################################################################################
# Automatically-generated file. Do not edit!
################################################################################

# Each subdirectory must supply rules for building sources it contributes
drivers/%.obj: ../drivers/%.c $(GEN_OPTS) | $(GEN_FILES) $(GEN_MISC_FILES)
	@echo 'Building file: "$<"'
	@echo 'Invoking: Arm Compiler'
	"/opt/ti/ccs1240/ccs/tools/compiler/ti-cgt-arm_20.2.7.LTS/bin/armcl" -mv7M4 --code_state=16 --float_support=FPv4SPD16 -me --include_path="/opt/ti/ccs1240/ccs/tools/compiler/ti-cgt-arm_20.2.7.LTS/include" --include_path="/home/estudiante/workspace_v12/practica-TIVA-2026" --include_path="/home/estudiante/workspace_v12/practica-TIVA-2026/FreeRTOS/Source/include" --include_path="/home/estudiante/workspace_v12/practica-TIVA-2026/FreeRTOS/Source/portable/CCS/ARM_CM4F" --define=ccs="ccs" --define=DEBUG --define=PART_TM4C123GH6PM --define=TARGET_IS_BLIZZARD_RB1 --define=WANT_CMDLINE_HISTORY -g --gcc --diag_warning=225 --diag_wrap=off --display_error_number --abi=eabi --preproc_with_compile --preproc_dependency="drivers/$(basename $(<F)).d_raw" --obj_directory="drivers" $(GEN_OPTS__FLAG) "$(shell echo $<)"
	@echo 'Finished building: "$<"'
	@echo ' '


