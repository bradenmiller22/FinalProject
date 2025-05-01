#define F_CPU 16000000UL
#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/eeprom.h>  // Add EEPROM library
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
#define TOP_SCORES_COUNT 3       // Number of top scores to save
#define TIMING_COMPENSATION 90   // Compensation for timing inaccuracy (in ms)

// EEPROM address definitions
#define EEPROM_INIT_MARKER 0xAA  // Marker to check if EEPROM has been initialized
#define EEPROM_INIT_ADDR   0     // Address to store initialization marker
#define EEPROM_EASY_ADDR   2     // Starting address for easy difficulty high scores
#define EEPROM_MEDIUM_ADDR 14    // Starting address for medium difficulty high scores
#define EEPROM_HARD_ADDR   26    // Starting address for hard difficulty high scores

// Global variables
volatile uint8_t button_state = 0;        // Current button state
volatile uint8_t button_pressed = 0;      // Button press event flag
volatile uint8_t button_released = 0;     // Button release event flag
volatile uint8_t game_state = STATE_WAITING_TO_START;
uint16_t reaction_time = 0;               // Reaction time in milliseconds
uint16_t total_reaction_time = 0;         // Sum of all reaction times
uint16_t reaction_times[MAX_ROUNDS];      // Store each round's reaction time
uint8_t current_round = 0;                // Current game round
uint8_t difficulty_level = DIFFICULTY_MEDIUM; // Default difficulty
uint16_t green_light_timeout = 2500;      // Default timeout (will be set based on difficulty)
uint16_t top_scores[3][TOP_SCORES_COUNT]; // Store top scores for each difficulty

// NEW: Flag to control buzzer in timeout situation
volatile uint8_t timeout_buzzer_active = 0;

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
	
	// Detect falling edge (button release)
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

// EEPROM Functions
void init_eeprom() {
	// Check if EEPROM has been initialized already
	uint8_t init_marker = eeprom_read_byte((uint8_t*)EEPROM_INIT_ADDR);
	
	if (init_marker != EEPROM_INIT_MARKER) {
		// EEPROM not initialized, initialize with default values (9999 ms)
		for (uint8_t diff = 0; diff < 3; diff++) {
			uint16_t* addr = (uint16_t*)(diff == DIFFICULTY_EASY ? EEPROM_EASY_ADDR :
			diff == DIFFICULTY_MEDIUM ? EEPROM_MEDIUM_ADDR :
			EEPROM_HARD_ADDR);
			
			// Initialize top scores to 9999 (worst possible time)
			for (uint8_t i = 0; i < TOP_SCORES_COUNT; i++) {
				eeprom_write_word(addr + i, 9999);
			}
		}
		
		// Mark EEPROM as initialized
		eeprom_write_byte((uint8_t*)EEPROM_INIT_ADDR, EEPROM_INIT_MARKER);
	}
}

// Read top scores from EEPROM based on difficulty
void read_top_scores(uint8_t difficulty) {
	uint16_t* addr = (uint16_t*)(difficulty == DIFFICULTY_EASY ? EEPROM_EASY_ADDR :
	difficulty == DIFFICULTY_MEDIUM ? EEPROM_MEDIUM_ADDR :
	EEPROM_HARD_ADDR);
	
	// Read top scores
	for (uint8_t i = 0; i < TOP_SCORES_COUNT; i++) {
		top_scores[difficulty][i] = eeprom_read_word(addr + i);
	}
}

// Save a new score and reorganize top scores if needed
void update_top_scores(uint8_t difficulty, uint16_t new_score) {
	// First read current scores
	read_top_scores(difficulty);
	
	// Check if new score is better than any existing scores
	uint8_t insert_pos = TOP_SCORES_COUNT; // Default to not inserting
	
	for (uint8_t i = 0; i < TOP_SCORES_COUNT; i++) {
		if (new_score < top_scores[difficulty][i]) {
			insert_pos = i;
			break;
		}
	}
	
	// If new score is a top score
	if (insert_pos < TOP_SCORES_COUNT) {
		// Shift scores down
		for (uint8_t i = TOP_SCORES_COUNT - 1; i > insert_pos; i--) {
			top_scores[difficulty][i] = top_scores[difficulty][i - 1];
		}
		
		// Insert new score
		top_scores[difficulty][insert_pos] = new_score;
		
		// Save updated scores to EEPROM
		uint16_t* addr = (uint16_t*)(difficulty == DIFFICULTY_EASY ? EEPROM_EASY_ADDR :
		difficulty == DIFFICULTY_MEDIUM ? EEPROM_MEDIUM_ADDR :
		EEPROM_HARD_ADDR);
		
		for (uint8_t i = 0; i < TOP_SCORES_COUNT; i++) {
			eeprom_write_word(addr + i, top_scores[difficulty][i]);
		}
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

// MODIFIED: Added timeout_buzzer_active flag check
void non_blocking_delay(uint16_t ms) {
	for (uint16_t i = 0; i < ms; i++) {
		_delay_ms(1);
		
		// Update buzzer for red light penalties or timeout buzzer
		if ((button_state && game_state == STATE_COUNTDOWN) || timeout_buzzer_active) {
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
		green_light_timeout = 3000;  // 3 seconds for easy
		break;
		case DIFFICULTY_MEDIUM:
		green_light_timeout = 1500;  // 1.5 seconds for medium
		break;
		case DIFFICULTY_HARD:
		green_light_timeout = 500;   // 0.5 seconds for hard
		break;
		default:
		green_light_timeout = 1500;  // Default to medium
	}
}

// Apply timing compensation to reaction time
uint16_t compensate_timing(uint16_t raw_time) {
	return raw_time + TIMING_COMPENSATION;
}

int main(void) {
	// Initialize hardware
	pwm_init();
	OLED_Init();
	OLED_Clear();
	
	// Setup pins
	DDRB |= (1 << BUZZER_PIN);   // PB0 output for buzzer
	setup_button_interrupt();    // Configure button interrupt
	
	// Initialize EEPROM if needed
	init_eeprom();
	
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
				OLED_Printf("3 sec to respond");
				break;
				case DIFFICULTY_MEDIUM:
				OLED_Printf("MEDIUM MODE");
				OLED_SetCursor(1, 1);
				OLED_Printf("1.5 sec to respond");
				break;
				case DIFFICULTY_HARD:
				OLED_Printf("HARD MODE");
				OLED_SetCursor(1, 1);
				OLED_Printf("0.5 sec to respond");
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
				// Apply timing compensation to reaction time
				uint16_t compensated_time = compensate_timing(reaction_time);
				
				// Store reaction time (with compensation)
				reaction_times[current_round-1] = compensated_time;
				total_reaction_time += compensated_time;
				
				// Update reaction_time for display
				reaction_time = compensated_time;
				
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
				
				// MODIFIED: Set timeout buzzer flag ON before delay
				timeout_buzzer_active = 1;
				
				// Sound buzzer for timeout
				PORTB |= (1 << BUZZER_PIN);  // Buzzer ON (redundant but safe)
				non_blocking_delay(500);    // Sound buzzer for .5 second
				
				// MODIFIED: Turn off timeout buzzer flag after use
				timeout_buzzer_active = 0;
				PORTB &= ~(1 << BUZZER_PIN); // Buzzer OFF (redundant but safe)
				
				non_blocking_delay(1500);    // Additional delay to complete the 2-second pause
				
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
			
			// Check if this average is a new high score
			update_top_scores(difficulty_level, average);
			
			// Read top scores for current difficulty
			read_top_scores(difficulty_level);
			
			// Show top times from EEPROM for current difficulty
			OLED_SetCursor(2, 2);
			OLED_Printf("Top Times:");
			
			OLED_SetCursor(3, 3);
			OLED_Printf("1. %d ms", top_scores[difficulty_level][0]);
			
			OLED_SetCursor(4, 4);
			OLED_Printf("2. %d ms", top_scores[difficulty_level][1]);
			
			OLED_SetCursor(5, 5);
			OLED_Printf("3. %d ms", top_scores[difficulty_level][2]);
			
			
			OLED_SetCursor(6, 6);
			OLED_Printf("HOLD 3s to restart");
			
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
