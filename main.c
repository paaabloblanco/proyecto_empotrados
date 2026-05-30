//*****************************************************************************
//
// Codigo de partida comunicacion TIVA-QT
// Autores: Eva Gonzalez, Ignacio Herrero, Jose Manuel Cano
//
//  Estructura de aplicacion basica para el desarrollo de aplicaciones genericas
//  basada en la TIVA, en las que existe un intercambio de mensajes con un interfaz
//  gráfico (GUI) Qt.
//
//*****************************************************************************
#include <stdint.h>
#include <stdbool.h>
#include "inc/hw_memmap.h"
#include "inc/hw_types.h"
#include "inc/hw_ints.h"
#include "driverlib/gpio.h"
#include "driverlib/pin_map.h"
#include "driverlib/rom.h"
#include "driverlib/rom_map.h"
#include "driverlib/sysctl.h"
#include "driverlib/uart.h"
#include "driverlib/interrupt.h"
#include "utils/uartstdioMod.h"
#include "driverlib/adc.h"
#include "driverlib/timer.h"
#include "drivers/buttons.h"
#include "drivers/rgb.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "utils/cpu_usage.h"
#include "commands.h"
#include <serial2USBprotocol.h>
#include <usb_dev_serial.h>
#include "usb_messages_table.h"
#include "config.h"
#include "math.h"
#include "queue.h"
#include "event_groups.h"
#include "driverlib/pwm.h"

// ---------------------------------------------------------------------------
// Variables globales
// ---------------------------------------------------------------------------
uint32_t g_ui32CPUUsage;
uint32_t g_ui32SystemClock;

SemaphoreHandle_t mutexUSB;
SemaphoreHandle_t mutexUART;
SemaphoreHandle_t semaforo_freertos1;
SemaphoreHandle_t semaforo_freertos2;
SemaphoreHandle_t semaforo_contador;

TaskHandle_t handleProductora1;
TaskHandle_t handleProductora2;

EventGroupHandle_t flagseventos;
EventGroupHandle_t flag_control;
EventGroupHandle_t traza;

QueueHandle_t mailbox_freertos;
QueueHandle_t cola_freertos1;
QueueHandle_t cola_freertos2;
QueueSetHandle_t grupoColas;
QueueHandle_t mailbox_delay;
typedef struct {
    uint32_t delay;
    uint8_t  idProd;
    QueueHandle_t     cola;
    SemaphoreHandle_t sem;
} ParamsProductora;

typedef struct {
    uint32_t id;
    uint32_t idProd;
} DatosProductora;

ParamsProductora params1 = {3000, 1};
ParamsProductora params2 = {5000, 2};

// ---------------------------------------------------------------------------
// Error hook del driver library
// ---------------------------------------------------------------------------
#ifdef DEBUG
void __error__(char *pcFilename, uint32_t ulLine)
{
    while(1);
}
#endif

// ---------------------------------------------------------------------------
// Hooks de FreeRTOS
// ---------------------------------------------------------------------------
void vApplicationStackOverflowHook(TaskHandle_t pxTask, char *pcTaskName)
{
    (void)pxTask;
    (void)pcTaskName;
    while(1);
}

void vApplicationTickHook(void)
{
    static uint8_t ui8Count = 0;
    if (++ui8Count == 10)
    {
        g_ui32CPUUsage = CPUUsageTick();
        ui8Count = 0;
    }
}

void vApplicationIdleHook(void)
{
    SysCtlSleep();
}

void vApplicationMallocFailedHook(void)
{
    while(1);
}

// ---------------------------------------------------------------------------
// ISR: manejadora ADC
// ---------------------------------------------------------------------------
void manejadora(void)
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    uint32_t ui32Value;

    ADCIntClear(ADC0_BASE, 0);
    ADCSequenceDataGet(ADC0_BASE, 0, &ui32Value);
    xQueueOverwriteFromISR(mailbox_freertos, &ui32Value, &xHigherPriorityTaskWoken);
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

// ---------------------------------------------------------------------------
// ISR: manejador botón derecho  (debounce por timer one-shot)
// ---------------------------------------------------------------------------
void manejador_boton(void)
{
    GPIOIntClear(BUTTONS_GPIO_BASE, RIGHT_BUTTON);
    TimerEnable(TIMER4_BASE, TIMER_A);
}

// ---------------------------------------------------------------------------
// ISR: manejador Timer4  (reanuda productoras tras pulsar botón)
// ---------------------------------------------------------------------------
void manejador_timer4(void)
{
    TimerIntClear(TIMER4_BASE, TIMER_TIMA_TIMEOUT);
    xTaskResumeFromISR(handleProductora1);
    xTaskResumeFromISR(handleProductora2);
}

// ---------------------------------------------------------------------------
// Tarea: temperatura
//   Lee el mailbox con el valor ADC del sensor interno y envía la temperatura
//   al PC mediante un mensaje USB.
// ---------------------------------------------------------------------------
static portTASK_FUNCTION(temperatura, pvParameters)
{
    (void)pvParameters;

    ADCIntEnable(ADC0_BASE, 0);
    IntEnable(INT_ADC0SS0);

    uint32_t temp;
    PARAM_MENSAJE_TEMPERATURA g;
    uint8_t  pui8Frame[MAX_FRAME_SIZE];
    int32_t  i32Numdatos;

    while(1)
    {
        xQueueReceive(mailbox_freertos, &temp, portMAX_DELAY);

        // Fórmula conversión sensor temperatura interno TM4C
        g.gra = 147.5f - ((247.5f * (float)temp) / 4096.0f);

        i32Numdatos = create_frame(pui8Frame, MENSAJE_TEMPERATURA, &g, sizeof(g), MAX_FRAME_SIZE);
        if (i32Numdatos >= 0)
        {
            xSemaphoreTake(mutexUSB, portMAX_DELAY);
            send_frame(pui8Frame, i32Numdatos);
            xSemaphoreGive(mutexUSB);
        }
    }
}

// ---------------------------------------------------------------------------
// Tarea: control
//   Espera eventos de flag_control:
//     bit 0 (0x01) -> alarma cola productora 1
//     bit 1 (0x02) -> alarma cola productora 2
//     bit 2 (0x04) -> objetivo cumplido (ensamblaje terminó)
// ---------------------------------------------------------------------------
static portTASK_FUNCTION(control, pvParameters)
{
    (void)pvParameters;

    uint8_t  pui8Frame[MAX_FRAME_SIZE];
    int32_t  i32Numdatos;
    PARAM_MENSAJE_ALARMA_COLA    parametro;
    PARAM_MENSAJE_OBJETIVO_CUMPLIDO c;
    EventBits_t flagsActivos;

    while(1)
    {
        // Bloquea hasta recibir cualquiera de los tres eventos; los borra al despertar
        flagsActivos = xEventGroupWaitBits(flag_control,
                                           0x01 | 0x02 | 0x04,
                                           pdTRUE,   // borrar bits al salir
                                           pdFALSE,  // basta con uno
                                           portMAX_DELAY);

        // --- Identificar qué alarma de cola llegó ---
        if (flagsActivos & 0x01)
        {
            parametro.cola = 1;
            if (xEventGroupGetBits(traza) & 0x01)
            {
                xSemaphoreTake(mutexUART, portMAX_DELAY);
                UARTprintf("Alarma: cola productora 1 llena\r\n");
                xSemaphoreGive(mutexUART);
            }
        }
        else if (flagsActivos & 0x02)
        {
            parametro.cola = 2;
            if (xEventGroupGetBits(traza) & 0x01)
            {
                xSemaphoreTake(mutexUART, portMAX_DELAY);
                UARTprintf("Alarma: cola productora 2 llena\r\n");
                xSemaphoreGive(mutexUART);
            }
        }

        // --- Objetivo cumplido: suspender productoras y notificar al PC ---
        if (flagsActivos & 0x04)
        {
            vTaskSuspend(handleProductora1);
            vTaskSuspend(handleProductora2);

            if (xEventGroupGetBits(traza) & 0x01)
            {
                xSemaphoreTake(mutexUART, portMAX_DELAY);
                UARTprintf("Objetivo cumplido: productoras suspendidas\r\n");
                xSemaphoreGive(mutexUART);
            }

            c.cumplido = true;
            i32Numdatos = create_frame(pui8Frame, MENSAJE_OBJETIVO_CUMPLIDO,
                                       &c, sizeof(c), MAX_FRAME_SIZE);
            if (i32Numdatos >= 0)
            {
                xSemaphoreTake(mutexUSB, portMAX_DELAY);
                send_frame(pui8Frame, i32Numdatos);
                xSemaphoreGive(mutexUSB);
            }
        }
        else
        {
            // Era una alarma de cola: enviar mensaje al PC
            if (xEventGroupGetBits(traza) & 0x01)
            {
                xSemaphoreTake(mutexUART, portMAX_DELAY);
                UARTprintf("Enviando alarma de cola al PC\r\n");
                xSemaphoreGive(mutexUART);
            }

            i32Numdatos = create_frame(pui8Frame, MENSAJE_ALARMA_COLA,
                                       &parametro, sizeof(parametro), MAX_FRAME_SIZE);
            if (i32Numdatos >= 0)
            {
                xSemaphoreTake(mutexUSB, portMAX_DELAY);
                send_frame(pui8Frame, i32Numdatos);
                xSemaphoreGive(mutexUSB);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Tarea: productora  (instanciada dos veces con params1 / params2)
//   Genera objetos con id aleatorio y los mete en la cola correspondiente.
//   Si la cola está llena, enciende el LED rojo y avisa a la tarea control.
// ---------------------------------------------------------------------------
static portTASK_FUNCTION(productora, pvParameters)
{
    ParamsProductora *params = (ParamsProductora *)pvParameters;
    TickType_t xDelay = params->delay / portTICK_PERIOD_MS;

    // Espera a que el GUI envíe MENSAJE_INICIO antes de producir
    xEventGroupWaitBits(flagseventos, 0x01, pdFALSE, pdTRUE, portMAX_DELAY);

    while(1)
    {
        DatosProductora obj;
        obj.id    = (uint32_t)(rand() % 10 + 1);
        obj.idProd = params->idProd;
        TickType_t nuevo_delay;
        if(xQueuePeek(mailbox_delay, &nuevo_delay, 0) == pdTRUE){
            xDelay = nuevo_delay;
        }



        vTaskDelay(xDelay);

        if (xQueueSend(params->cola, &obj, 0) != pdTRUE)
        {
            // Cola llena: LED rojo encendido + señal de alarma
            GPIOPinWrite(GPIO_PORTF_BASE, GPIO_PIN_1, GPIO_PIN_1);
            xSemaphoreGive(params->sem);

            if (params->idProd == 1)
                xEventGroupSetBits(flag_control, 0x01);
            else
                xEventGroupSetBits(flag_control, 0x02);
        }
        else
        {
            // Envío correcto: LED rojo apagado
            GPIOPinWrite(GPIO_PORTF_BASE, GPIO_PIN_1, 0);
        }
    }
}

// ---------------------------------------------------------------------------
// Tarea: ensamblaje
//   Consume objetos de ambas colas (a través del queue set).
//   Decrementa el semáforo contador; cuando llega a 0, señala objetivo cumplido.
// ---------------------------------------------------------------------------
static portTASK_FUNCTION(ensamblaje, pvParameters)
{
    (void)pvParameters;

    static uint32_t contador = 0;
    uint8_t  pui8Frame[MAX_FRAME_SIZE];
    int32_t  i32Numdatos;
    DatosProductora    nuevo;
    PARAM_MENSAJE_CONTADOR_PRODUCTOS parametro;
    TickType_t xDelay;

    // Espera señal de inicio
    xEventGroupWaitBits(flagseventos, 0x01, pdFALSE, pdTRUE, portMAX_DELAY);

    while(1)
    {
        // Espera a que haya al menos un producto pendiente de ensamblar
        xSemaphoreTake(semaforo_contador, portMAX_DELAY);

        // Selecciona la cola que tiene datos
        QueueHandle_t colaActiva = xQueueSelectFromSet(grupoColas, portMAX_DELAY);
        xQueueReceive(colaActiva, &nuevo, 0);

        // El tiempo de ensamblaje depende del origen del producto
        if (colaActiva == cola_freertos1)
            xDelay = 3000 / portTICK_PERIOD_MS;
        else
            xDelay = 5000 / portTICK_PERIOD_MS;

        vTaskDelay(xDelay);

        // Actualizar y enviar contador al GUI
        contador++;
        parametro.contador = contador;
        parametro.id       = nuevo.id;
        parametro.idProd   = nuevo.idProd;

        i32Numdatos = create_frame(pui8Frame, MENSAJE_CONTADOR_PRODUCTOS,
                                   &parametro, sizeof(parametro), MAX_FRAME_SIZE);
        if (i32Numdatos >= 0)
        {
            xSemaphoreTake(mutexUSB, portMAX_DELAY);
            send_frame(pui8Frame, i32Numdatos);
            xSemaphoreGive(mutexUSB);
        }

        // Comprobar si era el último producto del objetivo
        if (uxSemaphoreGetCount(semaforo_contador) == 0)
        {
            xEventGroupSetBits(flag_control, 0x04);
            vTaskSuspend(NULL);   // Se suspende a sí misma
        }
    }
}

// ---------------------------------------------------------------------------
// Tarea: USBMessageProcessingTask
//   Recibe tramas del GUI Qt y las despacha según el tipo de mensaje.
// ---------------------------------------------------------------------------
static portTASK_FUNCTION(USBMessageProcessingTask, pvParameters)
{
    uint8_t  pui8Frame[MAX_FRAME_SIZE];
    int32_t  i32Numdatos;
    uint8_t  ui8Message;
    void    *ptrtoreceivedparam;
    uint32_t ui32Errors = 0;

    (void)pvParameters;

    xSemaphoreTake(mutexUART, portMAX_DELAY);
    UARTprintf("\n\nBienvenido a la aplicacion Fabrica Automatizada (curso 2025/26)!\n");
    UARTprintf("Autores: XXXXXX y XXXXX\n");
    xSemaphoreGive(mutexUART);

    for(;;)
    {
        i32Numdatos = receive_frame(pui8Frame, MAX_FRAME_SIZE);
        if (i32Numdatos > 0)
        {
            i32Numdatos = destuff_and_check_checksum(pui8Frame, i32Numdatos);
            if (i32Numdatos < 0)
            {
                ui32Errors++;   // Checksum erróneo, descartar
            }
            else
            {
                ui8Message = decode_message_type(pui8Frame);

                if (xEventGroupGetBits(traza) & 0x01)
                {
                    xSemaphoreTake(mutexUART, portMAX_DELAY);
                    UARTprintf("MSG recibido: 0x%02X\r\n", ui8Message);
                    xSemaphoreGive(mutexUART);
                }

                i32Numdatos = get_message_param_pointer(pui8Frame, i32Numdatos,
                                                        &ptrtoreceivedparam);
                switch(ui8Message)
                {
                    // ----------------------------------------------------------
                    case MENSAJE_PING:
                        i32Numdatos = create_frame(pui8Frame, ui8Message, 0, 0, MAX_FRAME_SIZE);
                        if (i32Numdatos >= 0)
                        {
                            xSemaphoreTake(mutexUSB, portMAX_DELAY);
                            send_frame(pui8Frame, i32Numdatos);
                            xSemaphoreGive(mutexUSB);
                        }
                        else
                        {
                            ui32Errors++;
                            switch(i32Numdatos)
                            {
                                case PROT_ERROR_NOMEM:                  break;
                                case PROT_ERROR_STUFFED_FRAME_TOO_LONG: break;
                                case PROT_ERROR_MESSAGE_TOO_LONG:       break;
                                case PROT_ERROR_INCORRECT_PARAM_SIZE:   break;
                                default:                                break;
                            }
                        }
                        break;

                    // ----------------------------------------------------------
                    case MENSAJE_INICIO:
                        MAP_PWMGenEnable(PWM0_BASE, PWM_GEN_0);
                        MAP_PWMOutputState(PWM0_BASE, PWM_OUT_0_BIT, true);
                        xEventGroupSetBits(flagseventos, 0x01);
                        break;

                    // ----------------------------------------------------------
                    case MENSAJE_OBJETIVO_INICIO:
                    {
                        PARAM_MENSAJE_OBJETIVO_INICIO obj;
                        if (check_and_extract_message_param(ptrtoreceivedparam,
                                                            i32Numdatos,
                                                            sizeof(obj), &obj) > 0)
                        {
                            int i;
                            for (i = 0; i < obj.objetivo; i++)
                                xSemaphoreGive(semaforo_contador);
                        }
                        break;
                    }

                    // ----------------------------------------------------------
                    default:
                    {
                        PARAM_MENSAJE_NO_IMPLEMENTADO parametro;
                        parametro.message = ui8Message;
                        i32Numdatos = create_frame(pui8Frame, MENSAJE_NO_IMPLEMENTADO,
                                                   &parametro, sizeof(parametro), MAX_FRAME_SIZE);
                        if (i32Numdatos >= 0)
                        {
                            xSemaphoreTake(mutexUSB, portMAX_DELAY);
                            send_frame(pui8Frame, i32Numdatos);
                            xSemaphoreGive(mutexUSB);
                        }
                        break;
                    }
                } // switch
            }
        }
        else
        {
            ui32Errors++;   // Error de recepción de trama
        }
    }
}

// ---------------------------------------------------------------------------
// main()
// ---------------------------------------------------------------------------
int main(void)
{
    // Reloj del sistema a 40 MHz
    MAP_SysCtlClockSet(SYSCTL_SYSDIV_5 | SYSCTL_USE_PLL |
                       SYSCTL_XTAL_16MHZ | SYSCTL_OSC_MAIN);
    g_ui32SystemClock = SysCtlClockGet();

    MAP_SysCtlPeripheralClockGating(true);
    CPUUsageInit(g_ui32SystemClock, configTICK_RATE_HZ / 10, 3);

    // --- GPIO F: LED rojo (PIN_1) ---
    MAP_SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOF);
    MAP_GPIOPinTypeGPIOOutput(GPIO_PORTF_BASE, GPIO_PIN_1);
    MAP_GPIOPinWrite(GPIO_PORTF_BASE, GPIO_PIN_1, 0);

    // --- PWM0: señal de 10 Hz, ciclo 50 % en PB6 ---
    MAP_SysCtlPeripheralEnable(SYSCTL_PERIPH_PWM0);
    MAP_SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOB);
    while(!SysCtlPeripheralReady(SYSCTL_PERIPH_PWM0));
    while(!SysCtlPeripheralReady(SYSCTL_PERIPH_GPIOB));

    GPIOPinConfigure(GPIO_PB6_M0PWM0);
    GPIOPinTypePWM(GPIO_PORTB_BASE, GPIO_PIN_6);
    MAP_PWMGenConfigure(PWM0_BASE, PWM_GEN_0,
                        PWM_GEN_MODE_DOWN | PWM_GEN_MODE_NO_SYNC);
    MAP_PWMGenPeriodSet(PWM0_BASE, PWM_GEN_0, 4000000);   // 40 MHz / 4 M = 10 Hz
    MAP_PWMPulseWidthSet(PWM0_BASE, PWM_OUT_0, 2000000);  // 50 %

    // --- Timer 2: disparo periódico para el ADC (1 s) ---
    MAP_SysCtlPeripheralEnable(SYSCTL_PERIPH_TIMER2);
    MAP_SysCtlPeripheralSleepEnable(SYSCTL_PERIPH_TIMER2);
    TimerConfigure(TIMER2_BASE, TIMER_CFG_PERIODIC);
    TimerLoadSet(TIMER2_BASE, TIMER_A, 40000000);   // 1 s a 40 MHz

    // --- ADC0: sensor interno, disparado por Timer2 ---
    MAP_SysCtlPeripheralEnable(SYSCTL_PERIPH_ADC0);
    MAP_SysCtlPeripheralSleepEnable(SYSCTL_PERIPH_ADC0);
    ADCSequenceConfigure(ADC0_BASE, 0, ADC_TRIGGER_TIMER, 0);
    ADCSequenceStepConfigure(ADC0_BASE, 0, 0,
                             ADC_CTL_TS | ADC_CTL_END | ADC_CTL_IE);
    while(!SysCtlPeripheralReady(SYSCTL_PERIPH_ADC0));
    ADCSequenceEnable(ADC0_BASE, 0);
    ADCIntRegister(ADC0_BASE, 0, manejadora);

    TimerControlTrigger(TIMER2_BASE, TIMER_A, true);
    TimerEnable(TIMER2_BASE, TIMER_A);

    // --- Botón derecho: interrupción por flanco de bajada ---
    MAP_SysCtlPeripheralEnable(BUTTONS_GPIO_PERIPH);
    MAP_SysCtlPeripheralSleepEnable(BUTTONS_GPIO_PERIPH);
    GPIOIntRegister(BUTTONS_GPIO_BASE, manejador_boton);
    GPIOPinTypeGPIOInput(BUTTONS_GPIO_BASE, RIGHT_BUTTON);
    GPIOIntTypeSet(BUTTONS_GPIO_BASE, RIGHT_BUTTON, GPIO_FALLING_EDGE);
    GPIOIntEnable(BUTTONS_GPIO_BASE, RIGHT_BUTTON);
    IntPrioritySet(INT_GPIOF, configMAX_SYSCALL_INTERRUPT_PRIORITY);

    // --- Timer 4: one-shot 20 s (debounce / retardo reanudación) ---
    MAP_SysCtlPeripheralEnable(SYSCTL_PERIPH_TIMER4);
    MAP_SysCtlPeripheralSleepEnable(SYSCTL_PERIPH_TIMER4);
    TimerConfigure(TIMER4_BASE, TIMER_CFG_ONE_SHOT);
    TimerLoadSet(TIMER4_BASE, TIMER_A, 40000000 * 20);   // 20 s
    TimerIntRegister(TIMER4_BASE, TIMER_A, manejador_timer4);
    IntPrioritySet(INT_TIMER4A, configMAX_SYSCALL_INTERRUPT_PRIORITY);
    TimerIntEnable(TIMER4_BASE, TIMER_TIMA_TIMEOUT);

    // --- Recursos IPC ---
    semaforo_freertos1 = xSemaphoreCreateBinary();
    if (semaforo_freertos1 == NULL) while(1);

    semaforo_freertos2 = xSemaphoreCreateBinary();
    if (semaforo_freertos2 == NULL) while(1);

    semaforo_contador = xSemaphoreCreateCounting(100, 0);
    if (semaforo_contador == NULL) while(1);

    mutexUART = xSemaphoreCreateMutex();
    if (mutexUART == NULL) while(1);

    mutexUSB = xSemaphoreCreateMutex();
    if (mutexUSB == NULL) while(1);

    cola_freertos1 = xQueueCreate(3, sizeof(DatosProductora));
    if (cola_freertos1 == NULL) while(1);

    cola_freertos2 = xQueueCreate(3, sizeof(DatosProductora));
    if (cola_freertos2 == NULL) while(1);

    grupoColas = xQueueCreateSet(6);
    if (grupoColas == NULL) while(1);

    params1.cola = cola_freertos1;
    params2.cola = cola_freertos2;
    xQueueAddToSet(cola_freertos1, grupoColas);
    xQueueAddToSet(cola_freertos2, grupoColas);

    params1.sem = semaforo_freertos1;
    params2.sem = semaforo_freertos2;

    flagseventos = xEventGroupCreate();
    if (flagseventos == NULL) while(1);

    traza = xEventGroupCreate();
    if (traza == NULL) while(1);

    mailbox_freertos = xQueueCreate(1, sizeof(uint32_t));
    if (mailbox_freertos == NULL) while(1);
    mailbox_delay = xQueueCreate(1,sizeof(TickType_t));
    if(mailbox_delay == NULL) while(1);

    flag_control = xEventGroupCreate();
    if (flag_control == NULL) while(1);

    // --- Creación de tareas ---
    if (initCommandLine(256, tskIDLE_PRIORITY + 1) != pdPASS) while(1);

    USBSerialInit(32, 32);

    if (xTaskCreate(USBMessageProcessingTask, "usbser", 512,
                    NULL, tskIDLE_PRIORITY + 2, NULL) != pdPASS) while(1);

    if (xTaskCreate(productora, "produce", 256,
                    &params1, tskIDLE_PRIORITY + 1, &handleProductora1) != pdPASS) while(1);

    if (xTaskCreate(productora, "produce2", 256,
                    &params2, tskIDLE_PRIORITY + 1, &handleProductora2) != pdPASS) while(1);

    if (xTaskCreate(ensamblaje, "ensambla", 256,
                    NULL, tskIDLE_PRIORITY + 1, NULL) != pdPASS) while(1);

    if (xTaskCreate(temperatura, "temp", 256,
                    NULL, tskIDLE_PRIORITY + 1, NULL) != pdPASS) while(1);

    if (xTaskCreate(control, "cont", 256,
                    NULL, tskIDLE_PRIORITY + 1, NULL) != pdPASS) while(1);

    // Arranca el planificador (no retorna)
    vTaskStartScheduler();

    while(1); // No debería llegar aquí
}
