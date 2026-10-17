# Имя вашего главного C-файла без расширения (например, main.c -> main)
TARGET = main

# Модель микроконтроллера
MCU = atmega8

# Частота тактования (по умолчанию 1 МГц для зав. ATmega8)
F_CPU = 8000000UL

# Пути к компилятору
CC = "C:/Program Files (x86)/Atmel/AVR Tools/AVR Toolchain/bin/avr-gcc"
OBJCOPY = "C:/Program Files (x86)/Atmel/AVR Tools/AVR Toolchain/bin/avr-objcopy"

# Флаги компиляции
CFLAGS = -mmcu=$(MCU) -DF_CPU=$(F_CPU) -O2 -Wall

all: $(TARGET).hex

$(TARGET).hex: $(TARGET).elf
	$(OBJCOPY) -O ihex -R .eeprom $(TARGET).elf $(TARGET).hex

$(TARGET).elf: $(TARGET).c
	$(CC) $(CFLAGS) $(TARGET).c -o $(TARGET).elf

clean:
	del /q *.elf *.hex *.o
