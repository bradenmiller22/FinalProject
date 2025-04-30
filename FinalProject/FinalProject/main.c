#define F_CPU 16000000UL
#include <avr/io.h>
#include <avr/interrupt.h>
#include <util/delay.h>
#include <stdlib.h>
#include "i2c.h"
#include "SSD1306.h"

// Pin mappings
#define BUZZER_PIN 0   // PB0
#define BUTTON_PIN 2   // PD2 (INT0)

// Game states
#define STATE_WAITING_TO_START 0  // Initial state, waiting for button press to start
#define STATE_DIFFICULTY_SELECT 1  // Select difficulty level
#define STATE_COUNTDOWN        2  // Red light, waiting random time
#define STATE_GREEN_LIGHT      3  // Green light, waiting for button press
#define STATE_RESULT           4  // Show results, back to waiting
#define STATE_GAME_OVER        5  // Show final results after 5 rounds
#define STATE_LOSE             6  // Player pressed during red light, game over

// Difficulty levels
#define DIFFICULTY_EASY   0
#define DIFFICULTY_MEDIUM 1
#define DIFFICULTY_HARD   2

// Game settings
#define MAX_ROUNDS 5             // Number of rounds before game over

// Global variables
volatile uint8_t button_state = 0;        // Current button state
volatile uint8_t button_pressed = 0;      // Button press event flag
volatile uint8_t button_released = 0;     // Button release event flag - NEW VARIABLE
volatile uint8_t game_state = STATE_WAITING_TO_START;
uint16_t reaction_time = 0;               // Reaction time in milliseconds
uint16_t total_reaction_time = 0;         // Sum of all reaction times
uint16_t reaction_times[MAX_ROUNDS];      // Store each round's reaction time
uint8_t current_round = 0;                // Current game round
uint8_t difficulty_level = DIFFICULTY_MEDIUM; // Default difficulty
uint16_t green_light_timeout = 2500;      // Default timeout (will be set based on difficulty)

// Interrupt service routine for button press/release
ISR(INT0_vect) {
	// Small debounce delay
	_delay_ms(10);
	
	// Read button state (LOW when pressed due to pull-up)
	uint8_t new_button_state = !(PIND & (1 << BUTTON_PIN));
	
	// Detect rising edge (button press)
	if (new_button_state && !button_state) {
		// Only set button_pressed flag if we're not in the results screen
		// And don't register button presses during game over hold detection
		if (game_state != STATE_RESULT) {
			button_pressed = 1;
		}
	}
	
	// Detect falling edge (button release) - NEW CODE
	if (!new_button_state && button_state) {
		button_released = 1;
	}
	
	button_state = new_button_state;
	
	// Sound buzzer if button is pressed during red light (penalty)
	if (button_state && game_state == STATE_COUNTDOWN) {
		PORTB |= (1 << BUZZER_PIN);  // Buzzer ON
		} else {
		PORTB &= ~(1 << BUZZER_PIN); // Buzzer OFF
	}
}

void pwm_init() {
	TCCR1A = (1 << COM1A1) | (1 << COM1B1) | (1 << WGM10);
	TCCR1B = (1 << CS10);
	OCR1A = 0;
	OCR1B = 0;
	TCCR2A = (1 << COM2A1) | (1 << WGM20);
	TCCR2B = (1 << CS20);
	OCR2A = 0;
	DDRB |= (1 << 1) | (1 << 2) | (1 << 3);
}

void set_rgb(uint8_t r, uint8_t g, uint8_t b) {
	OCR1A = r;
	OCR1B = g;
	OCR2A = b;
}

void non_blocking_delay(uint16_t ms) {
	for (uint16_t i = 0; i < ms; i++) {
		_delay_ms(1);
		
		// Update buzzer for red light penalties
		if (button_state && game_state == STATE_COUNTDOWN) {
			PORTB |= (1 << BUZZER_PIN);  // Buzzer ON
			} else {
			PORTB &= ~(1 << BUZZER_PIN); // Buzzer OFF
		}
	}
}

// Rainbow effect for the start screen
void smooth_color_cycle() {
	for (uint16_t i = 0; i < 256; i++) {
		set_rgb(i, 0, 255-i);
		non_blocking_delay(5);
		if (button_pressed && game_state == STATE_WAITING_TO_START) {
			return; // Exit if button pressed during start screen
		}
	}
	for (uint16_t i = 0; i < 256; i++) {
		set_rgb(255-i, i, 0);
		non_blocking_delay(5);
		if (button_pressed && game_state == STATE_WAITING_TO_START) {
			return;
		}
	}
	for (uint16_t i = 0; i < 256; i++) {
		set_rgb(0, 255-i, i);
		non_blocking_delay(5);
		if (button_pressed && game_state == STATE_WAITING_TO_START) {
			return;
		}
	}
	for (uint16_t i = 0; i < 256; i++) {
		set_rgb(i, 0, 255-i);
		non_blocking_delay(5);
		if (button_pressed && game_state == STATE_WAITING_TO_START) {
			return;
		}
	}
}

void setRed() {
	set_rgb(255, 0, 0);
}

void setGreen() {
	set_rgb(0, 255, 0);
}

void setBlue() {
	set_rgb(0, 0, 255);
}

void setOrange() {
	set_rgb(255, 165, 0);
}

void setYellow() {
	set_rgb(255, 255, 0);
}

void setPurple() {
	set_rgb(128, 0, 128);
}

void setup_button_interrupt() {
	// Set PD2 as input with pull-up
	DDRD &= ~(1 << BUTTON_PIN);
	PORTD |= (1 << BUTTON_PIN);
	
	// Configure INT0 to trigger on ANY logic change (both press and release)
	EICRA |= (1 << ISC00);  // ISC01=0, ISC00=1: any logical change
	EICRA &= ~(1 << ISC01);
	
	// Enable INT0 interrupt
	EIMSK |= (1 << INT0);
	
	// Clear interrupt flag
	EIFR |= (1 << INTF0);
}

// Get random delay between 1-3 seconds (1000-3000 ms)
uint16_t get_random_delay() {
	return (rand() % 2000) + 1000;
}

// Reset game state for a new game
void reset_game() {
	current_round = 0;
	total_reaction_time = 0;
	for (uint8_t i = 0; i < MAX_ROUNDS; i++) {
		reaction_times[i] = 0;
	}
}

// Set green light timeout based on difficulty level
void set_difficulty(uint8_t level) {
	difficulty_level = level;
	
	switch (level) {
		case DIFFICULTY_EASY:
		green_light_timeout = 5000;  // 5 seconds for easy
		break;
		case DIFFICULTY_MEDIUM:
		green_light_timeout = 2500;  // 2.5 seconds for medium
		break;
		case DIFFICULTY_HARD:
		green_light_timeout = 1000;  // 1 second for hard
		break;
		default:
		green_light_timeout = 2500;  // Default to medium
	}
}

int main(void) {
	// Initialize hardware
	pwm_init();
	OLED_Init();
	OLED_Clear();
	
	// Setup pins
	DDRB |= (1 << BUZZER_PIN);   // PB0 output for buzzer
	setup_button_interrupt();    // Configure button interrupt
	
	// Seed random number generator
	srand(42);  // Fixed seed for now
	
	// Enable global interrupts
	sei();
	
	// Reset game state
	reset_game();
	
	// Main game loop
	while (1) {
		switch(game_state) {
			case STATE_WAITING_TO_START:
			// Display startup screen
			OLED_Clear();
			OLED_SetCursor(0, 0);
			OLED_Printf("Welcome!");
			OLED_SetCursor(1, 1);
			OLED_Printf("Press Button to Start");
			OLED_SetCursor(2, 2);
			OLED_Printf("5-Round Challenge!");
			
			// Rainbow effect during start screen
			button_pressed = 0;
			while (!button_pressed) {
				smooth_color_cycle();
			}
			
			// Go to difficulty select screen
			game_state = STATE_DIFFICULTY_SELECT;
			button_pressed = 0;
			button_released = 0; // Reset release flag
			break;
			


			case STATE_DIFFICULTY_SELECT:
			_delay_ms(100);
			// Display difficulty selection screen
			OLED_Clear();
			OLED_SetCursor(0, 0);
			OLED_Printf("Choose Difficulty:");
			OLED_SetCursor(2, 2);
			OLED_Printf("Press to cycle");
			
			uint8_t current_selection = 0;
			uint8_t selection_confirmed = 0;
			uint16_t selection_hold_time = 0;
			
			// Display initial difficulty
			OLED_SetCursor(1, 1);
			OLED_Printf("> EASY <");
			
			// Set yellow color for selection screen
			setYellow();
			
			// Wait for selection and confirmation
			while (!selection_confirmed) {
				// If button is RELEASED (not pressed), cycle through difficulties
				// This is the key change - we only cycle when the button is released
				if (button_released) {
					current_selection = (current_selection + 1) % 3;
					
					// Update display
					OLED_SetCursor(1, 1);
					switch (current_selection) {
						case 0:
						OLED_Printf("> EASY <  ");
						break;
						case 1:
						OLED_Printf("> MEDIUM <");
						break;
						case 2:
						OLED_Printf("> HARD <  ");
						break;
					}
					
					button_released = 0;  // Reset the release flag
				}
				
				// Check if button is being held to confirm selection
				if (button_state) {
					selection_hold_time += 10;
					
					// Only show progress if held longer than 300ms (to avoid flickering during quick presses)
					if (selection_hold_time > 300) {
						// Show progress at bottom of screen
						if (selection_hold_time % 100 == 0) {
							OLED_SetCursor(3, 3);
							OLED_Printf("Hold to confirm:%d%%", (selection_hold_time - 300) / 17);
						}
					}
					
					// If held for 2 seconds, confirm selection
					if (selection_hold_time >= 2000) {
						selection_confirmed = 1;
					}
					} else {
					selection_hold_time = 0;
					// Clear the progress text when button is released before threshold
					if (selection_hold_time < 300) {
						OLED_SetCursor(3, 3);
						OLED_Printf("                    ");
					}
				}
				
				non_blocking_delay(10);
			}
			
			// Set difficulty based on selection
			set_difficulty(current_selection);
			_delay_ms(135);
			// Confirm message
			OLED_Clear();
			OLED_SetCursor(0, 0);
			switch (difficulty_level) {
				case DIFFICULTY_EASY:
				OLED_Printf("EASY MODE");
				OLED_SetCursor(1, 1);
				OLED_Printf("5 sec to respond");
				break;
				case DIFFICULTY_MEDIUM:
				OLED_Printf("MEDIUM MODE");
				OLED_SetCursor(1, 1);
				OLED_Printf("2.5 sec to respond");
				break;
				case DIFFICULTY_HARD:
				OLED_Printf("HARD MODE");
				OLED_SetCursor(1, 1);
				OLED_Printf("1 sec to respond");
				break;
			}
			
			// Show "Get Ready!" message
			OLED_SetCursor(3, 3);
			OLED_Printf("Get ready!");
			non_blocking_delay(2000);
			
			// Start the game
			reset_game();
			current_round = 1;
			game_state = STATE_COUNTDOWN;
			button_pressed = 0;
			button_released = 0; // Reset release flag
			break;
			


			case STATE_COUNTDOWN:
			// Display round info
			OLED_Clear();
			OLED_SetCursor(0, 0);
			OLED_Printf("Round %d of %d", current_round, MAX_ROUNDS);
			OLED_SetCursor(1, 1);
			OLED_Printf("Wait for GREEN light!");
			
			// Set LED to red
			setRed();
			
			// Wait random time (1-3 seconds)
			uint16_t wait_time = get_random_delay();
			for (uint16_t i = 0; i < wait_time; i++) {
				non_blocking_delay(1);
				
				// If button pressed during red light, game over
				if (button_pressed) {
					game_state = STATE_LOSE;
					break;
				}
			}
			
			// If we successfully completed countdown without button press
			if (game_state == STATE_COUNTDOWN) {
				// Transition to green light
				game_state = STATE_GREEN_LIGHT;
				reaction_time = 0;
				button_pressed = 0;
			}
			break;
			


			case STATE_LOSE:
			// Show "YOU LOSE" message
			OLED_Clear();
			OLED_SetCursor(0, 0);
			OLED_Printf("YOU LOSE!");
			OLED_SetCursor(1, 1);
			OLED_Printf("Pressed during RED");
			OLED_SetCursor(3, 3);
			OLED_Printf("Press button");
			OLED_SetCursor(4, 4);
			OLED_Printf("to try again");
			
			// Set LED to red to indicate failure
			setRed();
			
			// Wait for button press to restart
			button_pressed = 0;
			while (!button_pressed) {
				non_blocking_delay(100);
			}
			
			// Reset everything and go back to start
			button_pressed = 0;
			button_released = 0; // Reset release flag
			reset_game();
			game_state = STATE_WAITING_TO_START;
			break;
			


			case STATE_GREEN_LIGHT:
			// Green light is on, start counting reaction time
			setGreen();
			OLED_Clear();
			OLED_SetCursor(0, 0);
			OLED_Printf("GREEN! Press button!");
			
			// Wait for button press or timeout based on difficulty
			while (!button_pressed && reaction_time < green_light_timeout) {
				non_blocking_delay(1);
				reaction_time++;
			}
			
			// Process results
			if (button_pressed) {
				// Store reaction time
				reaction_times[current_round-1] = reaction_time;
				total_reaction_time += reaction_time;
				game_state = STATE_RESULT;
				} else {
				// Timeout - too slow
				OLED_Clear();
				OLED_SetCursor(0, 0);
				OLED_Printf("Too slow!");
				OLED_SetCursor(1, 1);
				OLED_Printf("You lose!");
				
				// Set LED to red to indicate failure
				setRed();
				
				non_blocking_delay(2000);
				
				// Go back to start screen
				button_pressed = 0;
				button_released = 0; // Reset release flag
				reset_game();
				game_state = STATE_WAITING_TO_START;
			}
			
			button_pressed = 0;
			break;
			


			case STATE_RESULT:
			// Show results
			setBlue();
			OLED_Clear();
			OLED_SetCursor(0, 0);
			OLED_Printf("Round %d: %d ms", current_round, reaction_time);
			OLED_SetCursor(1, 1);
			
			// Give feedback based on reaction time
			if (reaction_time < 200) {
				OLED_Printf("Amazing! ");
				} else if (reaction_time < 400) {
				OLED_Printf("Great! ");
				} else if (reaction_time < 600) {
				OLED_Printf("Good! ");
				} else if (reaction_time < 800) {
				OLED_Printf("Eh! ");
				} else {
				OLED_Printf("Yea ur bad ");
			}
			
			// Wait before next round - ignoring button presses during this time
			non_blocking_delay(3000);
			
			// Make sure button_pressed flag is cleared before moving to next state
			button_pressed = 0;
			button_released = 0; // Reset release flag
			
			// Check if this was the last round
			if (current_round >= MAX_ROUNDS) {
				game_state = STATE_GAME_OVER;
				} else {
				// Set up next round
				current_round++;
				game_state = STATE_COUNTDOWN;
			}
			break;
			


			case STATE_GAME_OVER:
			// Show final results
			OLED_Clear();
			setOrange();
			
			// Calculate average reaction time
			uint16_t average = total_reaction_time / MAX_ROUNDS;
			
			// Show difficulty level that was played
			OLED_SetCursor(0, 0);
			switch (difficulty_level) {
				case DIFFICULTY_EASY:
				OLED_Printf("Completed on EASY!");
				break;
				case DIFFICULTY_MEDIUM:
				OLED_Printf("Completed on MEDIUM!");
				break;
				case DIFFICULTY_HARD:
				OLED_Printf("Completed on HARD!");
				break;
			}
			
			OLED_SetCursor(1, 1);
			OLED_Printf("Average Time: %d ms", average);

			//show top times from eeprom for that diffuculty
			OLED_SetCursor(2, 2);
			switch (difficulty_level) {
				case DIFFICULTY_EASY:
				OLED_Printf("Top Times on EASY:");
				break;
				case DIFFICULTY_MEDIUM:
				OLED_Printf("Top Times on MEDIUM:");
				break;
				case DIFFICULTY_HARD:
				OLED_Printf("Top Times on HARD:");
				break;
			}

			OLED_SetCursor(3, 3);
			OLED_Printf("1. "); //top time

			OLED_SetCursor(4, 4);
			OLED_Printf("2. "); //2nd time

			OLED_SetCursor(5, 5);
			OLED_Printf("3. "); //3rd time
			
			OLED_SetCursor(6, 6);
			OLED_Printf("HOLD 3s to restart: ");
			
			// Wait for button press
			button_pressed = 0;
			while (!button_pressed) {
				non_blocking_delay(100);
			}
			button_pressed = 0;
			
			// Now check if button is held for 3 seconds
			uint16_t restart_hold_time = 0;
			uint8_t showing_progress = 0;
			OLED_SetCursor(7, 7);
			
			while (restart_hold_time < 3000) {
				if (!(PIND & (1 << BUTTON_PIN))) {  // Button is being held (active low)
					restart_hold_time += 10;
					
					// Show progress every 300ms (10% progress)
					if (restart_hold_time % 300 < 10 && !showing_progress) {
						showing_progress = 1;
						uint8_t progress = restart_hold_time / 300;
						OLED_SetCursor(7, 7);
						
						// Show progress bar
						OLED_Printf("[");
						for (uint8_t i = 0; i < 10; i++) {
							if (i < progress) {
								OLED_Printf("=");
								} else {
								OLED_Printf(" ");
							}
						}
						OLED_Printf("]");
					}
					else if (restart_hold_time % 300 >= 10) {
						showing_progress = 0;
					}
					} else {
					// Button released - reset hold time
					restart_hold_time = 0;
					OLED_SetCursor(7, 7);
					OLED_Printf("[          ]");  // Clear progress bar
				}
				
				non_blocking_delay(10);
			}
			
			// Button held for 3 seconds, go back to start screen
			OLED_SetCursor(7, 7);
			OLED_Printf("Restarting...");
			non_blocking_delay(1000);
			
			button_pressed = 0;
			button_released = 0; // Reset release flag
			game_state = STATE_WAITING_TO_START;
			break;
		}
	}
	
	return 0;
}