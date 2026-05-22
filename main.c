//*****************************************************************************
//
// Codigo de partida comunicacion TIVA-QT
// Autores: Eva Gonzalez, Ignacio Herrero, Jose Manuel Cano
//
//  Estructura de aplicacion basica para el desarrollo de aplicaciones genericas
//  basada en la TIVA, en las que existe un intercambio de mensajes con un interfaz
//  gráfico (GUI) Qt.
//  La aplicacion se basa en un intercambio de mensajes con ordenes e informacion, a traves  de la
//  configuracion de un perfil CDC de USB (emulacion de puerto serie) y un protocolo
//  de comunicacion con el PC que permite recibir ciertas ordenes y enviar determinados datos en respuesta.
//   En el ejemplo basico de partida se implementara la recepcion de un mensaje
//  generico que permite el apagado y encendido de los LEDs de la placa; asi como un segundo
//  mensaje enviado desde la placa al GUI, para mostrar el estado de los botones.
//
//*****************************************************************************
#include <stdint.h>
#include <stdbool.h>
#include "inc/hw_memmap.h"       // TIVA: Definiciones del mapa de memoria
#include "inc/hw_types.h"        // TIVA: Definiciones API
#include "inc/hw_ints.h"         // TIVA: Definiciones para configuracion de interrupciones
#include "driverlib/gpio.h"      // TIVA: Funciones API de GPIO
#include "driverlib/pin_map.h"   // TIVA: Mapa de pines del chip
#include "driverlib/rom.h"       // TIVA: Funciones API incluidas en ROM de micro (ROM_)
#include "driverlib/rom_map.h"   // TIVA: Para usar la opción MAP en las funciones API (MAP_)
#include "driverlib/sysctl.h"    // TIVA: Funciones API control del sistema
#include "driverlib/uart.h"      // TIVA: Funciones API manejo UART
#include "driverlib/interrupt.h" // TIVA: Funciones API manejo de interrupciones
#include "utils/uartstdioMod.h"  // TIVA: Funciones API UARTSTDIO (printf)
#include "driverlib/adc.h"       // TIVA: Funciones API manejo de ADC
#include "driverlib/timer.h"     // TIVA: Funciones API manejo de timers
#include "drivers/buttons.h"     // TIVA: Funciones API manejo de botones
#include "drivers/rgb.h"         // TIVA: Funciones API manejo de leds con PWM
#include "FreeRTOS.h"            // FreeRTOS: definiciones generales
#include "task.h"                // FreeRTOS: definiciones relacionadas con tareas
#include "semphr.h"              // FreeRTOS: definiciones relacionadas con semaforos
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
#include "adc.h"


// Variables globales "main"
uint32_t g_ui32CPUUsage;
uint32_t g_ui32SystemClock;
SemaphoreHandle_t mutexUSB, mutexUART, semaforo_freertos1;  // Para proteccion del canal USB y el caal UART -terminal-, ya que ahora lo van a usar varias tareas distintas

EventGroupHandle_t flagseventos;
QueueSetHandle_t grupoColas;
QueueHandle_t cola_freertos1;
QueueHandle_t cola_freertos2;
typedef struct {
    uint32_t delay;
    uint8_t idProd;
    QueueHandle_t cola;
} ParamsProductora;
typedef struct {
    uint32_t id;
    uint32_t idProd;
}DatosProductora;

ParamsProductora params1 = {3000, 1};
ParamsProductora params2 = {5000, 2};
//*****************************************************************************
//
// The error routine that is called if the driver library encounters an error.
//
//*****************************************************************************
#ifdef DEBUG
void __error__(char *pcFilename, uint32_t ulLine)
{
    while(1) //Si la ejecucion esta aqui dentro, es que el RTOS o alguna de las bibliotecas de perifericos han comprobado que hay un error
    { //Mira el arbol de llamadas en el depurador y los valores de nombrefich y linea para encontrar posibles pistas.
    }
}
#endif

//*****************************************************************************
//
// Aqui incluimos los "ganchos" a los diferentes eventos del FreeRTOS
//
//*****************************************************************************
void vApplicationStackOverflowHook(TaskHandle_t pxTask, char *pcTaskName)
{
    //
    // This function can not return, so loop forever.  Interrupts are disabled
    // on entry to this function, so no processor interrupts will interrupt
    // this loop.
    //
    while(1)
    {
    }
}

void vApplicationTickHook( void )
{
    static uint8_t ui8Count = 0;

    if (++ui8Count == 10)
    {
        g_ui32CPUUsage = CPUUsageTick();
        ui8Count = 0;
    }
    //return;
}

void vApplicationIdleHook (void)
{
    SysCtlSleep();
}

void vApplicationMallocFailedHook (void)
{
    while(1);
}

//*****************************************************************************
//
// A continuacion van las tareas...
//
//*****************************************************************************



static portTASK_FUNCTION(productora,pvParameters){
    ParamsProductora *params = (ParamsProductora *)pvParameters;
    const TickType_t delay = params->delay /portTICK_PERIOD_MS;


    xEventGroupWaitBits(flagseventos, 0x01, pdFALSE, pdTRUE, portMAX_DELAY);
    while(1){

        DatosProductora obj;
        obj.id = (rand()%10+1);
        obj.idProd = params->idProd;

        vTaskDelay(delay);
      //  xSemaphoreGive(semaforo_freertos1 );
        if(xQueueSend(params->cola,&obj,0)!= pdTRUE){
            GPIOPinWrite(GPIO_PORTF_BASE, GPIO_PIN_1, GPIO_PIN_1);

        }else{
            GPIOPinWrite(GPIO_PORTF_BASE, GPIO_PIN_1, 0);
        }


    }

}

static portTASK_FUNCTION(ensamblaje, pvParameters){
    (void) pvParameters;
    static uint32_t contador = 0;
            uint8_t pui8Frame[MAX_FRAME_SIZE];
            int32_t i32Numdatos;
            DatosProductora new;
            PARAM_MENSAJE_CONTADOR_PRODUCTOS parametro;
         TickType_t delay ;
        xEventGroupWaitBits(flagseventos, 0x01, pdFALSE, pdTRUE, portMAX_DELAY);
        while(1){

           QueueHandle_t colaActiva =  xQueueSelectFromSet(grupoColas,portMAX_DELAY);
           xQueueReceive(colaActiva, &new, 0);
           if(colaActiva == cola_freertos1){
               delay = 3000 /portTICK_PERIOD_MS;
           }else{
               delay = 5000 /portTICK_PERIOD_MS;
           }
           vTaskDelay(delay);
           contador++;
           parametro.contador = contador;
           parametro.id = new.id;
           parametro.idProd = new.idProd;
           i32Numdatos = create_frame(pui8Frame, MENSAJE_CONTADOR_PRODUCTOS, &parametro, sizeof(parametro), MAX_FRAME_SIZE);
           if(i32Numdatos >= 0){
               xSemaphoreTake(mutexUSB, portMAX_DELAY);
                  send_frame(pui8Frame, i32Numdatos);
                  xSemaphoreGive(mutexUSB);
           }
        }

}
//*****************************************************************************
//
// Codigo de tarea que procesa los mensajes recibidos a traves del canal USB
//
//*****************************************************************************
static portTASK_FUNCTION( USBMessageProcessingTask, pvParameters ){

    uint8_t pui8Frame[MAX_FRAME_SIZE];
    int32_t i32Numdatos;
    uint8_t ui8Message;
    void *ptrtoreceivedparam;
    uint32_t ui32Errors=0;

    /* The parameters are not used. */
    ( void ) pvParameters;

    //
    // Mensaje de bienvenida inicial.
    //
    xSemaphoreTake(mutexUART, portMAX_DELAY);
    UARTprintf("\n\nBienvenido a la aplicacion Fabrica Automatizada (curso 2025/26)!\n");
    UARTprintf("\nAutores: XXXXXX y XXXXX ");
    xSemaphoreGive(mutexUART);

    for(;;)
    {
        //Espera hasta que se reciba una trama con datos serializados por el interfaz USB
        i32Numdatos=receive_frame(pui8Frame,MAX_FRAME_SIZE); //Esta funcion es bloqueante
        if (i32Numdatos>0)
        {	//Si no hay error, proceso la trama que ha llegado.
            i32Numdatos=destuff_and_check_checksum(pui8Frame,i32Numdatos); // Primero, "destuffing" y comprobación checksum
            if (i32Numdatos<0)
            {
                //Error de checksum (PROT_ERROR_BAD_CHECKSUM), ignorar el paquete
                ui32Errors++;
                // Procesamiento del error 
            }
            else
            { //El paquete esta bien, luego procedo a tratarlo.
                ui8Message=decode_message_type(pui8Frame); //Obtiene el valor del campo mensaje
                i32Numdatos=get_message_param_pointer(pui8Frame,i32Numdatos,&ptrtoreceivedparam); //Obtiene un puntero al campo de parametros y su tamanio.
                switch(ui8Message)
                {
                case MENSAJE_PING :
                    //A un mensaje de ping se responde con el propio mensaje
                    i32Numdatos=create_frame(pui8Frame,ui8Message,0,0,MAX_FRAME_SIZE);
                    if (i32Numdatos>=0)
                    {
                        xSemaphoreTake(mutexUSB,portMAX_DELAY);
                        send_frame(pui8Frame,i32Numdatos);
                        xSemaphoreGive(mutexUSB);
                    }else{
                        //Error de creacion de trama: determinar el error y abortar operacion
                        ui32Errors++;
                        // Procesamiento del error
                        // Esto de aqui abajo podria ir en una funcion "createFrameError(numdatos)  para evitar
                        // tener que copiar y pegar todo en cada operacion de creacion de paquete
                        switch(i32Numdatos){
                        case PROT_ERROR_NOMEM:
                            // Procesamiento del error NO MEMORY
                            break;
                        case PROT_ERROR_STUFFED_FRAME_TOO_LONG:
                            // Procesamiento del error STUFFED_FRAME_TOO_LONG
                            break;
                        case PROT_ERROR_MESSAGE_TOO_LONG:
                            // Procesamiento del error MESSAGE TOO LONG
                            break;
                        case PROT_ERROR_INCORRECT_PARAM_SIZE:
                            // Procesamiento del error INCORRECT PARAM SIZE 
                            break;
                        }
                    }
                    break;
                case MENSAJE_INICIO:
                   MAP_PWMGenEnable(PWM0_BASE, PWM_GEN_0);
                    MAP_PWMOutputState(PWM0_BASE, PWM_OUT_0_BIT , true);
                    xEventGroupSetBits(flagseventos, 0x01);

                    break;
                default:
                {
                    PARAM_MENSAJE_NO_IMPLEMENTADO parametro;
                    parametro.message=ui8Message;
                    //El mensaje esta bien pero no esta implementado
                    i32Numdatos=create_frame(pui8Frame,MENSAJE_NO_IMPLEMENTADO,&parametro,sizeof(parametro),MAX_FRAME_SIZE);
                    if (i32Numdatos>=0)
                    {
                        xSemaphoreTake(mutexUSB,portMAX_DELAY);
                        send_frame(pui8Frame,i32Numdatos);
                        xSemaphoreGive(mutexUSB);
                    }
                    break;
                }
                }// switch
            }
        }else{ // if (ui32Numdatos >0)
            //Error de recepcion de trama(PROT_ERROR_RX_FRAME_TOO_LONG), ignorar el paquete
            ui32Errors++;
            // Procesamiento del error
        }
    }
}

//*****************************************************************************
//
// Funcion main(), Inicializa los perifericos, crea las tareas, etc... y arranca el bucle del sistema
//
//*****************************************************************************
int main(void)
{

    //
    // Reloj del sistema definido a 40MHz
    //
    MAP_SysCtlClockSet(SYSCTL_SYSDIV_5 | SYSCTL_USE_PLL | SYSCTL_XTAL_16MHZ | SYSCTL_OSC_MAIN);

    // Obtiene el reloj del sistema
    g_ui32SystemClock = SysCtlClockGet();

    //Habilita el clock gating de los perifericos durante el bajo consumo --> perifericos que se desee activos en modo Sleep
    //                                                                        deben habilitarse con SysCtlPeripheralSleepEnable
    MAP_SysCtlPeripheralClockGating(true);

    // Inicializa el subsistema de medida del uso de CPU (mide el tiempo que la CPU no esta dormida)
    // Para eso utiliza un timer, que aqui hemos puesto que sea el TIMER3 (ultimo parametro que se pasa a la funcion)
    // (y por tanto este no se deberia utilizar para otra cosa).
    CPUUsageInit(g_ui32SystemClock, configTICK_RATE_HZ/10, 3);
    //Inicializacion del puerto F par los leds
    MAP_SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOF);
    MAP_GPIOPinTypeGPIOOutput(GPIO_PORTF_BASE, GPIO_PIN_1);
    MAP_GPIOPinWrite(GPIO_PORTF_BASE, GPIO_PIN_1, 0);

    //inicializacion modulo PWM0
     MAP_SysCtlPeripheralEnable(SYSCTL_PERIPH_PWM0);
     MAP_SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOB);
     while(!SysCtlPeripheralReady(SYSCTL_PERIPH_PWM0));
     while(!SysCtlPeripheralReady(SYSCTL_PERIPH_GPIOB));

     GPIOPinConfigure(GPIO_PB6_M0PWM0);
     GPIOPinTypePWM(GPIO_PORTB_BASE, GPIO_PIN_6);
     MAP_PWMGenConfigure(PWM0_BASE,PWM_GEN_0,PWM_GEN_MODE_DOWN | PWM_GEN_MODE_NO_SYNC);
     //Para 10 hz,  periodo = 1 /10, periodo por 40 Mh = 40000000
     MAP_PWMGenPeriodSet(PWM0_BASE,PWM_GEN_0,4000000);
     //Para ciclo de trabajo 50 por ciento
     MAP_PWMPulseWidthSet(PWM0_BASE, PWM_OUT_0, 2000000);


     //timer adc
     MAP_SysCtlPeripheralEnable(SYSCTL_PERIPH_TIMER2));
     MAP_SysCtlPeripheralSleepEnable(SYSCTL_PERIPH_TIMER2);
     TimerConfigure(TIMER2_BASE, TIMER_CFG_PERIODIC);
     T
     //adc
     MAP_SysCtlPeripheralEnable(SYSCTL_PERIPH_ADC0);
     while(!SysCtlPeripheralReady(SYSCTL_PERIPH_ADC0)){

     }

    semaforo_freertos1 = xSemaphoreCreateBinary();
        if(semaforo_freertos1 == NULL) {
            while(1);  //no hay memoria para los semaforo

        }
        mutexUART=xSemaphoreCreateMutex();
          if(NULL == mutexUART)
              while(1);

          mutexUSB=xSemaphoreCreateMutex();
          if(NULL == mutexUSB)
              while(1);
          cola_freertos1= xQueueCreate(3,sizeof(DatosProductora));
          cola_freertos2= xQueueCreate(3,sizeof(DatosProductora));


          grupoColas= xQueueCreateSet(6);
          params1.cola = cola_freertos1;
          params2.cola = cola_freertos2;
          xQueueAddToSet(cola_freertos1, grupoColas);
          xQueueAddToSet(cola_freertos2, grupoColas);

     flagseventos = xEventGroupCreate();
          if(NULL == flagseventos){
              while(1);
          }


    /**                                              Creacion de tareas 									**/
    // Inicializa el sistema de depuración e interprete de comandos por terminal UART
    if (initCommandLine(256,tskIDLE_PRIORITY + 1) != pdPASS)
    {
        while(1);
    }

    USBSerialInit(32,32);	//Inicializo el  sistema USB
    //
    // Crea la tarea que gestiona los mensajes USB (definidos en USBMessageProcessingTask)
    //
    if(xTaskCreate(USBMessageProcessingTask,"usbser",512, NULL, tskIDLE_PRIORITY + 2, NULL) != pdPASS)
    {
        while(1);
    }

    //
    // A partir de aqui se crean las tareas de usuario, y los recursos IPC que se vayan a necesitar
    //
    if(xTaskCreate(productora, "produce", 256,&params1, tskIDLE_PRIORITY + 1, NULL) != pdPASS){
        while(1);
    }
    if(xTaskCreate(productora, "produce2", 256,&params2, tskIDLE_PRIORITY + 1, NULL) != pdPASS){
            while(1);
        }
    if(xTaskCreate(ensamblaje, "ensambla", 256,NULL, tskIDLE_PRIORITY + 1, NULL) != pdPASS){

           while(1);
       }


    //
    // Pone en marcha el planificador. La llamada NO tiene retorno
    //

    vTaskStartScheduler();	//el RTOS habilita las interrupciones al entrar aqui, asi que no hace falta habilitarlas

    //De la funcion vTaskStartScheduler no se sale nunca... a partir de aqui pasan a ejecutarse las tareas.
    while(1)
    {
        //Si llego aqui es que algo raro ha pasado
    }
}

