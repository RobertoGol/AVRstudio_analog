#ifndef F_CPU
#define F_CPU 8000000UL
#endif

#include <avr/io.h>
#include <util/delay.h>

int main(void) {
    DDRB |= (1 << PB0);
    while (1) {
        PORTB |= (1 << PB0);
        
        //led_ind
        _delay_ms(500);
        PORTB &= ~(1 << PB0); 
        _delay_ms(500);
    }
    return 0;
}
