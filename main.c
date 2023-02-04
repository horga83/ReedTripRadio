/*
    Alternative firmware for STC15W101/4 processor + SYN115 radio transmitter door/window reed sensor(s)
    (see README)
 */
#include "project-defs.h"

#include <delay.h>

// just do not have much more space for HAL with 1KB flash code size
//#include <eeprom-hal.h>
//#include <gpio-hal.h>
//#include <power-hal.h>
//#include <timer-hal.h>

#ifndef MCU_HAS_WAKE_UP_TIMER
    // Shouldn't happen, unless using an STC12.
    #error "The selected MCU doesn't have a power-down wake-up timer."
#endif // MCU_HAS_WAKE_UP_TIMER

// uncomment only one protocol timing
//#define PROTOCOL_STOCK
#define PROTOCOL_ONE

#if 1
    // it is difficult to stay under 1KB code space with all features present so use conditional compilation
    // you probably do not want to enable interrupts and heartbeats because it can cause duplicate codes (or miss events)
    //   and it takes up too much code space
    //
    // default is to pick least frequent wake up time and disable radio heartbeats to save power
    #define INTERRUPTS_ENABLED        true
    // this is intended to keep transmitting if someone is fooling with housing (tamper is opened), until tamper is again closed
    #define TAMPER_TRIP_ENABLED       false
    // heart beat is for those that want to be sure the door sensor is operational and is sending fresh state regularly
    #define REED_HEARTBEAT_ENABLED    false
    // only enable tamper heartbeat or tamper trip, not both
    #define TAMPER_HEARTBEAT_ENABLED  false
    // note that the interrupt to detect battery low is always enabled currently
    //   but this is if you really need to know battery low/ok periodically
    #define BATTERY_HEARTBEAT_ENABLED false
#else
    #define INTERRUPTS_ENABLED        false
    #define TAMPER_TRIP_ENABLED       false
    #define REED_HEARTBEAT_ENABLED    true
    #define TAMPER_HEARTBEAT_ENABLED  true
    #define BATTERY_HEARTBEAT_ENABLED true
#endif

#if 1
    // check for tamper switch pressed at startup, if so enter bootloader mode
    // this is helpful during firmware development or reflashing
    // if inserting the battery, make sure not to hold down the tamper switch
    // FIXME: concerned that brownout reset could falsely trigger this while in case
    #define CHECK_FOR_TAMPER_AT_STARTUP true
#else
    #define CHECK_FOR_TAMPER_AT_STARTUP false
#endif

#if 0
    // use upper bits of code as packet count to help in seeing missing packets
    #define SEND_PACKET_COUNT_DEBUG true
#else
    #define SEND_PACKET_COUNT_DEBUG false
#endif


// hardware pin definitions
//   circuit is voltage divider and high side transistor with processor controlling divider
//   pin 3.0 is also connected with receive pin on header 
#define RADIO_VDD      P3_0
// LED is attached to P3.1 for door sensor RF 433 MHz (STC15W101 and STC15W104 processor, SYN115 RF transmitter)
// pin 3.1 is also connected with transmit pin on header
#define LED_PIN        P3_1
#define REED_SWITCH    P3_2
#define TAMPER_SWITCH  P3_3
// ASK modulation to RF chip
#define RADIO_ASK      P3_4
// on my board when variable power supply (stand in for battery)
//   reaches just above one volt from starting point of 1.5 volts
//   this signal line goes from high (3.3V) to low (0V)
#define BATTERY_DETECT P3_5


// radio protocol apparently requires repeated transmissions so it is accepted at receiver
// RF-Bridge-EFM8BB1 alternative firmware for receiver chip on sonoff bridge seems to require only repeating twice
// stock EFM8BB1 receiver on sonoff bridge r1 seems to require four retransmissions
// @mmotley999 reported up to eight retransmissions are required
// @bismosa reports stock firmware repeats transmission twenty times!
// sonoff bridge r2 seems to to similarly require many repeats
//#define REPEAT_TRANSMISSIONS 4
//#define REPEAT_TRANSMISSIONS 8
#define REPEAT_TRANSMISSIONS 20


// sec. 9, table datasheet output blanking VDD transition from low to high (500 microseconds)
// oscillator startup time crystal HC49S (300 microseconds)
// standby ask delay time (min, 30), typical(75), max(120) milliseconds
// however fig. 6 shows only about a two millisecond ramp up time for VDD
// sec. 14.2 of SYN115-ASK-Transmitter-Datasheet.pdf describes two more modes for on-off control that save battery power
// may want to look into that more in the future
// milliseconds
#define RADIO_STARTUP_TIME 10

// seems to be required so some receivers can handle multiple packets sent one after another
// milliseconds
#define RADIO_GUARD_TIME 20

// tens of microseconds
#define DEBOUNCE_TIME_10US 20

// maximum is 32768 as per sec. 7.8 power down wake-up special timer
// highest bit of register will be set to enable wake up timer
// maximum is 0x7FFF and then highest bit set

// pg. 551, sec. 7.8 power down wake up special timer
// this equates to about 16 seconds according to examples
// note: why does setting to 0xFFFF wake up too quickly, about every two seconds?
#define SLEEP_TIME_0 0xfffe

// milliseconds
#define CONTROLLER_STARTUP_TIME 200

// array size
#define EVENT_HISTORY_SIZE 16

// unique ID location in flash
//  1K MCU (e.g. STC15W101)
// #define ID_ADDR_ROM 0x03f9
//  4K MCU (eg. STC15W104)
//#define ID_ADDR_ROM 0x0ff9

// unique ID is stored in ram in the same locations on mcu 101/104 parts
// see makefile - we are limiting ram size so that it is not initialized at these addresses
// it is possible to pull id from flash space, but it is at different locations on different sized parts
// note: the values stored in ram at these locations should match the last four characters shown by Target UID when flashing
#define ID0_ADDR_RAM 0x76
#define ID1_ADDR_RAM 0x77


// point at ram locations to derive uniqe id for inclusion in radio messages as original firmware did
// see sec. 1.12 global unique identification number STC15-English.pdf
static const __idata unsigned char *guid0 = (__idata unsigned char*) ID0_ADDR_RAM;
static const __idata unsigned char *guid1 = (__idata unsigned char*) ID1_ADDR_RAM;


// codes used in original firmware
// (we leave upper bits available for possibly adding transmission count in future)
// low voltage code reported by https://community.home-assistant.io/t/budget-priced-wife-friendly-wireless-wall-switch-using-433mhz-and-mqtt/88281
// if sensor is being powered by header pins this will also show low apparently
static const unsigned char battery_low  = 0x06;
static const unsigned char tamper_open  = 0x07;
static const unsigned char reed_open    = 0x0A;
static const unsigned char reed_close   = 0x0E;
// added close code to support tamper closed
// note: if we resend only a generic tamper code (e.g., 0x07) too quickly due to any switch change (e.g., a press and then a release)
//       I think the duplicate code sent may be discarded at receiver
static const unsigned char tamper_close = 0x08;
// added battery okay code if anyone will use it
static const unsigned char battery_ok   = 0x09;


// changed pulse lengths from microseconds to 10 microseconds units
//   because hardware abstraction layer provides delay10us() 
// avoid performing multiplies on processor by computing these as constants
// stock protocol was read from unaltered sensor by user @bismosa using logic analyzer
// "protocol 1" is the first rc-switch protocol which also supported by RF-Bridge-EFM8BB1
// (https://github.com/Portisch/RF-Bridge-EFM8BB1/blob/master/inc/RF_Protocols.h)
// rc-switch project timings (https://github.com/sui77/rc-switch)
#if defined(PROTOCOL_STOCK)
    // stock sensor timings (see door_sensor_reverse_notes_fv1.txt)
    // (tested working on Sonoff Bridge R2 V1.0)
    // (does not work on stock Sonoff Bridge R2 V2.2)
    const uint16_t gPulseHigh =   47;
    const uint16_t gPulseLow  = 1406;
    const uint16_t gZeroHigh  =   47;
    const uint16_t gZeroLow   =  136;
    const uint16_t gOneHigh   =  136;
    const uint16_t gOneLow    =   47;
    
    // it saves code space to just specify polarity for conditional compilation
    #define PROTOCOL_INVERTED false

#elif defined(PROTOCOL_ONE)
    const uint16_t gPulseHigh =   35;
    const uint16_t gPulseLow  = 1085;
    const uint16_t gZeroHigh  =   35;
    const uint16_t gZeroLow   =  105;
    const uint16_t gOneHigh   =  105;
    const uint16_t gOneLow    =   35;
    
    #define PROTOCOL_INVERTED false

#endif // specify protocol


// set flags in interrupts, then save and send event history in main loop
struct Flags {
    volatile bool reedInterrupted;
    volatile bool tamperInterrupted;
    volatile bool lowBatteryInterrupted;
    volatile bool tamperTripped;
    // save switches or battery states for sending
    volatile unsigned char eventHistory[EVENT_HISTORY_SIZE];
    volatile unsigned char eventCount;
    // track packets sent since startup
    volatile unsigned char packetCount;
};

// arrays are not initialized, but okay because we only read when count is incremented
struct Flags flag = {
    .reedInterrupted       = false, 
    .tamperInterrupted     = false,
    .lowBatteryInterrupted = false,
    .tamperTripped         = false,
    .eventCount = 0
};



// only enable power to radio when we are going to modulate ASK pin (i.e., send data)
// low pin setting enables transistor which supplies power to radio chip
inline void enable_radio_vdd(void)
{
    RADIO_VDD = 0;
}

// pin setting functions are more readable than direct pin setting
// and avoid making errors (e.g., "enabling" something is actually setting pin equal to zero)
inline void disable_radio_vdd(void)
{
    RADIO_VDD = 1;
}

inline void radio_ask_first_logic_level()
{
    #if PROTOCOL_INVERTED
        RADIO_ASK = 1;
    #else
        RADIO_ASK = 0;
    #endif
}

// specify inline to save some flash code space
inline void radio_ask_second_logic_level()
{
    #if PROTOCOL_INVERTED
        RADIO_ASK = 0;
    #else
        RADIO_ASK = 1;
    #endif
}

// led is controlled by transistor which essentially inverts pin output
// (so low level enables transistor and then LED is on)
inline void led_on(void)
{
    LED_PIN = 0;
}

inline void led_off(void)
{
    LED_PIN = 1;
}

// external interrupt 0 (reed switch connected)
inline void enable_ext0(void)
{
    // default is to interrupt on both falling and rising edges (IT0 = 0)    
    // enable external interrupt 0
    IE1 |= M_EX0;
}

// external interrupt 1 (tamper switch connected)
inline void enable_ext1(void)
{
    // default is to interrupt on both falling and rising edges (IT1 = 0)  
    // enable external interrupt 1
    IE1 |= M_EX1;
}

// external interrupt 3 (battery low/ok connected)
inline void enable_ext3(void)
{
    // only able to interrupt on falling edge
    // enable external interrupt 3
    INT_CLKO |= M_EX3;
}

inline void enable_global_interrupts(void)
{
    // enable global interrupts
    EA = 1;
}

// allows integer delays with 10 microsecond function at the expense of accuracy
void delay10us_wrapper(unsigned int microseconds)
{
    const unsigned char step = 0xff;
    
    while (microseconds > step)
    {
        delay10us(step);
        microseconds -= step;
    }
    
    delay10us(microseconds);
}

/*! \brief 
 *
 */        
inline bool isTamperOpen(void)
{
    return TAMPER_SWITCH;
}

inline bool isReedOpen(void)
{    
    return REED_SWITCH;
}

// read resistive divider state
inline bool isBatteryLow(void)
{    
    return !BATTERY_DETECT;
}

void rfsyncPulse()
{
    // rf sync pulse
    radio_ask_second_logic_level();
    delay10us_wrapper(gPulseHigh);
    
    // this should be the only really long delay required
    radio_ask_first_logic_level();
    delay10us_wrapper(gPulseLow);
}

/*! \brief Description
 *         Tips [http://ww1.microchip.com/downloads/en/AppNotes/Atmel-9164-Manchester-Coding-Basics_Application-Note.pdf]
 *
 */  
void send(const unsigned char byte)
{
    const unsigned char numBits = 8;
    
    // mask out highest bit
    const unsigned char mask = 1 << (numBits - 1);
    
    unsigned char i = 0;

    // byte for shifting
    unsigned char toSend = byte;
    
    // Repeat until all bits sent
    for(i = 0; i < numBits; i++)
    {
        // mask out all but left most bit value, and if byte is not equal to zero (i.e. left most bit must be one) then send one level
        if((toSend & mask) > 0)
        {
            radio_ask_second_logic_level();
            delay10us_wrapper(gOneHigh);
            
            radio_ask_first_logic_level();
            delay10us_wrapper(gOneLow);
        }
        else
        {
            radio_ask_second_logic_level();
            delay10us_wrapper(gZeroHigh);
            
            radio_ask_first_logic_level();
            delay10us_wrapper(gZeroLow);
        }
        
        toSend = toSend << 1;
    }
}

void sendRadioPacket(const unsigned char rfcode)
{
    unsigned char index;
    
    enable_radio_vdd();
    

    delay1ms(RADIO_STARTUP_TIME);
    
    // many receivers require repeatedly sending identical transmissions to accept data
    for (index = 0; index < REPEAT_TRANSMISSIONS; index++)
    {
        rfsyncPulse();

        // send rf key with unique id and code
        send(*guid0);
        send(*guid1);
        
#if (SEND_PACKET_COUNT_DEBUG)
        unsigned char byteToSend;
        // wrap around every sixteen counts because we only have upper four bits in radio packet available
        byteToSend = ((flag.packetCount & 0x0f) << 4) | rfcode;
        send(byteToSend);
#else
        send(rfcode);
#endif
    }
    
    disable_radio_vdd();
    
    // FIXME: we need to force ask low/high just in case correct?
}


//-----------------------------------------
//FIXME: handle reentrancy?
// interrupt and wake up on reed pin change (default is rising and falling edge)
void external_isr0(void) __interrupt 0
{
    //flag.reedInterruptCount++;
    flag.reedInterrupted = true;
}

//-----------------------------------------
// interrupt and wake up on tamper switch pin change
void external_isr1(void) __interrupt 2
{
    //flag.tamperInterruptCount++;
    flag.tamperInterrupted = true;
}

//-----------------------------------------
// interrupt and wake up on battery state change
void external_isr3(void) __interrupt 11
{
    flag.lowBatteryInterrupted = true;
}

// sec. 4.1 All port pins default to quasi-bidirectional after reset. Each one has a Schmitt-triggered input for improved input noise rejection.
//batteryMonitor GPIO_PIN_CONFIG(GPIO_PORT3, GPIO_PIN5, GPIO_HIGH_IMPEDANCE_MODE)
//radioASK       GPIO_PIN_CONFIG(GPIO_PORT3, GPIO_PIN4, GPIO_BIDIRECTIONAL_MODE)
//tamperSwitch   GPIO_PIN_CONFIG(GPIO_PORT3, GPIO_PIN3, GPIO_BIDIRECTIONAL_MODE)
//reedSwitch     GPIO_PIN_CONFIG(GPIO_PORT3, GPIO_PIN2, GPIO_BIDIRECTIONAL_MODE)
//ledPin         GPIO_PIN_CONFIG(GPIO_PORT3, GPIO_PIN1, GPIO_OPEN_DRAIN_MODE)
//radioVDD       GPIO_PIN_CONFIG(GPIO_PORT3, GPIO_PIN0, GPIO_OPEN_DRAIN_MODE)
inline void configure_pin_modes(void)
{
    // avoid bit operations to save on flash code space
    // just explicitly set gpio mode
    P3M1 = 0x23;
    P3M0 = 0x03;
}

// open drain
//   turn led off
//   disable radio power
// sec. 4.9.1 quasi-bidirectional i/o
//  enables weak pullups for reed and tamper switches assuming we are in bidirectional mode (pins high)
// note: do not use radio_ask_X() to disable power, because it might be inverted, so would actually enable power!
inline void startup_pins_state(void)
{
    P3 = 0x0f;
}

void main(void)
{
    // as per HAL instructions
    INIT_EXTENDED_SFR();
    
    // makes the code a little cleaner below
    bool state;
    bool flagCheck;
    
    // makes accessing array element more readable below
    unsigned char index;

    // setup gpio
    configure_pin_modes();
    startup_pins_state();
    
    // pulse LED at startup
    led_on();
    
    // give the microcontroller time to stabilize
    delay1ms(CONTROLLER_STARTUP_TIME);
    
    led_off();


#if CHECK_FOR_TAMPER_AT_STARTUP

    // if we are holding down tamper switch at power up, enter iap mode
    // this is intended to make entering flash mode easier on subsequent flashes
    // if the board has no tamper switch, then this will just never do anything
    // if a regular user is inserting a battery, it is unlikely that they are also pressing tamper switch
    //  so this should just be skipped over in that case
    if (!isTamperOpen())
    {
        // set ISP boot bit and reset processor
        IAP_CONTR = 0x60;
    }
    
#endif // CHECK_FOR_TAMPER_AT_STARTUP
    

#if (INTERRUPTS_ENABLED)
    // we always enable tamper and reed interrupts in hardware
    // but might ignore them depending on settings above
    enable_ext0();
    enable_ext1();
    
    // enable low battery detect interrupt
    enable_ext3();
    
    // does exactly what the function name says
    enable_global_interrupts();
#endif


    // main loop
    while (true)
    {        
        // only enable wake up timer if periodic wake up required, otherwise clear/disable
        // note: sec. 3.3.1 special function registers address map shows that default state of WKTCx has default value in timer
        flagCheck  = TAMPER_HEARTBEAT_ENABLED;
        flagCheck |= REED_HEARTBEAT_ENABLED;
        flagCheck |= BATTERY_HEARTBEAT_ENABLED;
        flagCheck |= flag.tamperTripped;
        
        if (flagCheck)
        {
            // set wake up count and enable wake up timer bit
            WKTC = SLEEP_TIME_0;
        } else {
            // could just clear most significant enable bit, but guess no harm in clearing count also
            WKTC = 0x0000;
        }
        
        // make sure there is not an unserviced interrupt prior to sleeping
        flagCheck  = !flag.reedInterrupted;
        flagCheck &= !flag.tamperInterrupted;
        if (flagCheck)
        {
            {
                // go to sleep
                // program will either resume due to wake up timer (if enabled) or due to interrupt
                PCON |= M_PD;
                
                // sec. 2.3.3.1 demo program in datasheet and example HAL show providing nops() after power down
                NOP();
                NOP();
            }
        }
        
        
#if (INTERRUPTS_ENABLED)

        // if we do too much in the interrupts themselves it produces too many stack pushes/pops
        // among other problems that are caused, so perform most logic here
        if (flag.reedInterrupted)
        {
            
            flag.reedInterrupted = false;

            // slope on reed switch from high to low is about 200 microseconds as measured on oscilloscope
            // FIXME: this may be a terrible debounce
            delay10us(DEBOUNCE_TIME_10US);
         
            
            // more readable for array indexing
            index = flag.eventCount;
            
            // save any switch (or later battery) changes in order of occurence
            // note: we really do not have code space for checking array overflows
            if (isReedOpen())
            {
                flag.eventHistory[index] = reed_open;
            } else {
                flag.eventHistory[index] = reed_close;
            }
            
            // track count because we might have multiple events in quick succession
            flag.eventCount++;
            
        }

        
        // we do not check if reed occurred prior to tamper or vice versa but should not matter too much
        if (flag.tamperInterrupted)
        {            
    
            flag.tamperInterrupted = false;

            // FIXME: another terrible debounce
            delay10us(DEBOUNCE_TIME_10US);
            
            index = flag.eventCount;
            
            // just read switch once here, but use in two logic locations below
            state = isTamperOpen();
            
            if (state)
            {
                flag.eventHistory[index] = tamper_open;
            } else {
                flag.eventHistory[index] = tamper_close;
            }
            
            flag.eventCount++;
            
            

            // if tamper is open, consider it tripped until closed
            // note: if tamper trip is not enabled, this flag should not do anything
            if (state)
            {
                // flag as tripped for sending elsewhere
                flag.tamperTripped = true;
            }
        }
        

        // interrupt will only trigger once as battery discharges, unless voltage bounces around
        if (flag.lowBatteryInterrupted)
        {
            flag.lowBatteryInterrupted = false;

            if (isBatteryLow())
            {
                flag.eventHistory[flag.eventCount] = battery_low;
                flag.eventCount++;
            }
            
        }
        
#endif // INTERRUPTS_ENABLED
        
// only allow tamper heart beat mode if tamper trip mode is disabled
#if (!TAMPER_TRIP_ENABLED && TAMPER_HEARTBEAT_ENABLED)

        // force sending out periodic messages to indicate tamper state
        if (TAMPER_HEARTBEAT_ENABLED)
        {
            // send different keys depending on reading push button
            if (isTamperOpen())
            {            
                flag.eventHistory[flag.eventCount] = tamper_open;
            } else {
                flag.eventHistory[flag.eventCount] = tamper_close;
            }
            
            flag.eventCount++;
        }
        
#endif
        
// only allow tamper trip if tamper heart beat mode is disabled
#if (TAMPER_TRIP_ENABLED && !TAMPER_HEARTBEAT_ENABLED)
        
        // tamper trip is similar to heart beat mode, but does not send close states
        if (flag.tamperTripped)
        {
            // send different keys depending on reading push button
            if (isTamperOpen())
            {            
                flag.eventHistory[flag.eventCount] = tamper_open;
                flag.eventCount++;

            } else {
                flag.tamperTripped = false;
            }
            
        }
        
#endif
        
#if (REED_HEARTBEAT_ENABLED)

        // similar periodic message but for reed switch
        if (REED_HEARTBEAT_ENABLED)
        {
            if(isReedOpen())
            {
                flag.eventHistory[flag.eventCount] = reed_open;
            } else {
                flag.eventHistory[flag.eventCount] = reed_close;
            }
            
            flag.eventCount++;
        }
        
#endif


 #if (BATTERY_HEARTBEAT_ENABLED)
 
        if (BATTERY_HEARTBEAT_ENABLED)
        {
            if(isBatteryLow())
            {
                flag.eventHistory[flag.eventCount] = battery_low;
            } else {
                flag.eventHistory[flag.eventCount] = battery_ok;
            }
            
            flag.eventCount++;
        }
#endif
        
        
        // send any stored event(s) over radio if count is incremented
        // we need to start at index zero every time (the oldest event)
        index = 0;
        while (flag.eventCount > 0)
        {
            // this is smarter than using using blocking delays to pulse led
            led_on();
            
            // send radio packet consisting of start pulse and 24-bits of data
            sendRadioPacket(flag.eventHistory[index]);
            
            // delay before sending next packet so as not to overwhelm receiver
            delay1ms(RADIO_GUARD_TIME);
            
            // effectively just pulsed led due to inherent delays of radio sending
            led_off();
            
            // count up from zero (i.e., send oldest to newest event in that order)
            index++;
            flag.eventCount--;
            
            // track this for debugging purposes
            flag.packetCount++;
        }
        

    } // main while loop
}
