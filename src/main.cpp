#include <Arduino.h>
#include <EEPROM.h>

#define STROBE_PIN 7                      // pin the strobe light is connected to
#define STROBE_OFF PORTD &= ~0b10000000;  // turn strobe on (inverted) for pin 7
#define STROBE_ON PORTD |= 0b10000000;    // turn strobe off (inverted) for pin7

#define FREQ_LOCATION 0   // EEPROM address to store last frequency value
#define STEP_LOCATION 10  // EEPROM address to store step size
#define UNIT_LOCATION 20  // EEPROM address to store unit

#define COOLDOWN1 40    // cooldown for rpm up and down buttons
#define COOLDOWN2 100   // cooldown for half and double buttons
#define COOLDOWN3 1000  // cooldown for showing alternative info (step size, unit)

#define DISPLAY_SCROLL_SPEED 250

#define LONG_PRESS 200

// different frequency mode scalers
#define HZ 1
#define RPM 60

// struct for different prescalers (store masking bits)
typedef struct {
    int scaler;
    byte flags;
} prescaler_t;

// struct to store button names, pins and values
typedef struct {
    int up, dwn, hlf, dbl, spd;
} button_names_t;

typedef struct {
    union {
        button_names_t names;
        int arr[5] = {A2, A1, A0, A7, A6};
    } buttons;
    union {
        button_names_t names;
        int arr[5] = {0};
    } values;
} button_data_t;

// struct to store 7-segment display pins and segment names
typedef struct {
    int a, b, c, d, e, f, g, dp, d1, d2, d3, d4;
} segment_names_t;

// struct to store 7-segment digit and character names
typedef struct {
    int _0 = 0, _1 = 1, _2 = 2, _3 = 3, _4 = 4, _5 = 5, _6 = 6, _7 = 7, _8 = 8, _9 = 9, H = 10, R = 11, P = 12, M = 13, OFF = 14;
} digit_names_t;

typedef struct {
    union {
        segment_names_t names;
        int arr[12] = {A3, 6, 3, 4, 5, 13, 2, A4, 9, 10, 11, 12};
    } segments;

    digit_names_t digit_names;
    byte digits[15] = {
        0b11111100,  // 0
        0b01100000,  // 1
        0b11011010,  // 2
        0b11110010,  // 3
        0b01100110,  // 4
        0b10110110,  // 5
        0b10111110,  // 6
        0b11100000,  // 7
        0b11111110,  // 8
        0b11110110,  // 9
        0b01101110,  // H
        0b00001010,  // r
        0b11001110,  // P
        0b11101100,  // 'M'...
        0b00000000   //off
    };

} display_data_t;

button_data_t button_data;
display_data_t display_data;
prescaler_t prescalers[5];

//frequency and step get overwitten with EEPROM values
double freq = 100;  //always in centiHz
unsigned long step = 1;
unsigned int unit = RPM;

//stores current cooldown values
unsigned long cooldown = 0;
unsigned long display_scroll_cooldown = 0;
unsigned long reset_display_to_freq_cooldown = 0;

// actual number displayed on display
byte display_digits[10] = {0, 0, 1, 10, 10, 10, 10, 10, 10, 10};

// scroll values
int display_scroll_max = 0;
int display_scroll_current = 0;

// displays one digit on the 7-segment display
void setSegments(display_data_t *display, byte number, bool dp) {
    // or with decimal point bit if decimal argument is true
    byte number_bits = display->digits[number] | (dp ? 0b00000001 : 0b00000000);

    // loop through segments and set state
    for (int i = 0; i < 8; i++)
        digitalWrite(display->segments.arr[i], bitRead(number_bits, 7 - i));
}

void setDisplay(unsigned long display_number) {
    //convert number to decimal and store it in array, calculate maximum display offset
    for (int i = 0; i < 10; i++) {
        if (display_number > 0) {
            display_digits[i] = display_number % 10;
            display_number /= 10;
            display_scroll_max = i;
        } else if (i <= 2) {
            display_digits[i] = 0;
            display_scroll_max = i;
        } else {
            display_digits[i] = display_data.digit_names.OFF;
        }
    }

    display_scroll_max = max(1, display_scroll_max - 2);
}

// updates timers according to rpm
void updateTimer() {
    
    // start with smalest prescaler (index)
    int prescaler_index = 0;

    // top / trigger value for timer
    unsigned long long ocr = 0;

    // if time would overflow with current prescaler, choose the next (bigger) prescaler until it doesn't 
    // overflow 16bit (divide freq by 100 because of centiHz)
    while (prescaler_index < 4 && (ocr = (16000000 / prescalers[prescaler_index].scaler) / (freq / 100.0)) > 65535) {
        prescaler_index++;
    }

    // write ocr to 16bit variable for setting register
    uint16_t ocr_16bit = ocr;

    //some debug information
    Serial.println("Prescaler: " + String(prescalers[prescaler_index].scaler));
    Serial.println("Ocr:       " + String(ocr_16bit));

    cli();  // clear interupt enable bit -> disable interupts

    TCCR1A = 0;
    TCCR1B = 0;
    TCCR1B |= prescalers[prescaler_index].flags;  // set prescaler bits
    TCCR1B |= (1 << WGM12);                       // sets operation to CTC, top is OCR1A
    OCR1A = ocr;                                  // set top value

    sei();  // set interupt enable bit -> enable interupts
}

void setup() {
    // Serial for debugging information
    Serial.begin(9600);

    // set all button pins to input with pullup resistors
    for (int i = 0; i < 5; i++) pinMode(button_data.buttons.arr[i], INPUT_PULLUP);

    // set all display pins to output
    for (int i = 0; i < 12; i++) pinMode(display_data.segments.arr[i], OUTPUT);

    // pin with stobe light is output
    pinMode(STROBE_PIN, OUTPUT);

    // in case you want to reset EEPROM
    // EEPROM.put(FREQ_LOCATION, freq);
    // EEPROM.put(STEP_LOCATION, step);
    // EEPROM.put(UNIT_LOCATION, unit);

    // read rpm and step values from EEPROM address
    EEPROM.get(FREQ_LOCATION, freq);
    EEPROM.get(STEP_LOCATION, step);
    EEPROM.get(UNIT_LOCATION, unit);

    // fix broken EEPROM values
    if (freq < 1 || freq > 10000000)
        freq = 100;

    if (unit != HZ && unit != RPM)
        unit = HZ;

    // set up prescaler lookup array with masking bits and prescaler values
    prescalers[0].scaler = 1;
    prescalers[0].flags = (1 << CS10);

    prescalers[1].scaler = 8;
    prescalers[1].flags = (1 << CS11);

    prescalers[2].scaler = 64;
    prescalers[2].flags = (1 << CS11) | (1 << CS10);

    prescalers[3].scaler = 256;
    prescalers[3].flags = (1 << CS12);

    prescalers[4].scaler = 1024;
    prescalers[4].flags = (1 << CS12) | (1 << CS10);

    cli();  // disable timers
    
    TCCR1A = 0;
    TCCR1B = 0;
    TIMSK1 |= (1 << OCIE1A);  // enables TIME1_COMPA_vect for interupt handling

    // Timer_2 for turning of strobe
    TCCR2A = 0;
    TCCR2B = 0;
    TCCR2A |= (1 << WGM21);               // sets operation to CTC, top is OCR2A
    TCCR2B |= (1 << CS21) | (1 << CS20);  // sets timer two to 32 prescale
    OCR2A = 50;
    TIMSK2 |= (1 << OCIE2A);  // enables TIME2_COMPA_vect for interupt handling

    sei();  // enable timers

    // sets up timer1 for current rpm
    updateTimer();
    setDisplay(freq * unit);
}

void loop() {
    // decrement cooldown
    if (cooldown > 0) cooldown--;
    if (display_scroll_cooldown > 0) display_scroll_cooldown--;
    if (reset_display_to_freq_cooldown > 0) reset_display_to_freq_cooldown--;

    // read button states
    for (int i = 0; i < 5; i++) button_data.values.arr[i] = analogRead(button_data.buttons.arr[i]) > 512;

    // only react if the cooldown from the last butten press is 0
    if (cooldown == 0) {
        
        // set flag for updating the timer and EEPROM later
        bool save = true;

        if (button_data.values.names.up == 0) {  // if up button is pressed
            freq += step / (double)unit;         // increase frequency by step size
            cooldown = COOLDOWN1;                // start cooldown
            reset_display_to_freq_cooldown = 0;

        } else if (button_data.values.names.dwn == 0) {  // if down button is pressed
            freq = max(freq - step / (double)unit, 1);   // decrement frequency
            cooldown = COOLDOWN1;                        // start cooldown
            reset_display_to_freq_cooldown = 0;

        } else if (button_data.values.names.hlf == 0) {  // if half button pressed
            freq = max(freq / 2, 1);                     // half frequency
            cooldown = COOLDOWN2;                        // start cooldown
            reset_display_to_freq_cooldown = 0;

        } else if (button_data.values.names.dbl == 0) {  // if double button pressed
            freq = max(freq * 2, 1);                     // double frequency
            cooldown = COOLDOWN2;                        // start cooldown
            reset_display_to_freq_cooldown = 0;

        } else if (button_data.values.names.spd == 0) {  // if step button pressed
            unsigned long start_millis = millis();
            while (analogRead(button_data.buttons.names.spd) < 512)
                ;

            if (millis() - start_millis > LONG_PRESS) {
                if (unit == HZ) {
                    unit = RPM;
                    display_digits[3] = display_data.digit_names.R;
                    display_digits[2] = display_data.digit_names.P;
                    display_digits[1] = display_data.digit_names.M;
                    display_digits[0] = display_data.digit_names.OFF;
                } else {
                    unit = HZ;
                    freq = round(freq);
                    display_digits[3] = display_data.digit_names.H;
                    display_digits[2] = display_data.digit_names._2;
                    display_digits[1] = display_data.digit_names.OFF;
                    display_digits[0] = display_data.digit_names.OFF;
                }
                display_scroll_max = 1;
                display_scroll_cooldown = 0;

            } else {
                step *= 10;                      // set step to next stepsize
                if (step >= 10000000) step = 1;  // max stepsize is 100000.00

                display_scroll_current = 0;  // set display scroll to beginning of number
                setDisplay(step);
            }

            cooldown = COOLDOWN2;
            reset_display_to_freq_cooldown = COOLDOWN3;

        } else {
            save = false;  // if nothing changed we don't need to update EEPROM
        }

        // update timer and EEPROM if flag is set
        if (save) {
            EEPROM.put(FREQ_LOCATION, freq);
            EEPROM.put(STEP_LOCATION, step);
            updateTimer();
        }
    }

    if (reset_display_to_freq_cooldown == 0) {
        setDisplay(freq * unit);
    }

    // increment display scroll if cooldown is 0, reset cooldown
    if (display_scroll_cooldown == 0) {
        display_scroll_current = (display_scroll_current + 1) % display_scroll_max;
        display_scroll_cooldown = DISPLAY_SCROLL_SPEED;
    }

    // dont show tenths or hundrets if they are 0
    if (display_digits[1] == 0 && display_digits[0] == 0)
        display_scroll_current = display_scroll_max - 3;
    else if (display_digits[0] == 0)
        display_scroll_current = display_scroll_max - 2;

    // calculate display offset so 0 = no offset
    int display_offset = display_scroll_max - 1 - display_scroll_current;

    //draw number to display
    for (int i = 0; i < 4; i++) {
        pinMode(display_data.segments.names.d1, INPUT);
        pinMode(display_data.segments.names.d2, INPUT);
        pinMode(display_data.segments.names.d3, INPUT);
        pinMode(display_data.segments.names.d4, INPUT);

        setSegments(&display_data, display_digits[i + display_offset], i == 2 - display_offset);

        pinMode(display_data.segments.names.d1, i == 0 ? OUTPUT : INPUT);
        pinMode(display_data.segments.names.d2, i == 1 ? OUTPUT : INPUT);
        pinMode(display_data.segments.names.d3, i == 2 ? OUTPUT : INPUT);
        pinMode(display_data.segments.names.d4, i == 3 ? OUTPUT : INPUT);

        delay(1);
    }

    pinMode(display_data.segments.names.d1, INPUT);
    pinMode(display_data.segments.names.d2, INPUT);
    pinMode(display_data.segments.names.d3, INPUT);
    pinMode(display_data.segments.names.d4, INPUT);
}

// iterrupt service for timer1 turns on strobe and resets timer2
ISR(TIMER1_COMPA_vect) {
    TCNT2 = 0;     // set timer2 to 0
    TIFR2 = 0xFF;  // clear timer2 flags
    STROBE_ON;
}

// iterrupt service for timer2 turns off stobe
ISR(TIMER2_COMPA_vect) {
    STROBE_OFF;
}
