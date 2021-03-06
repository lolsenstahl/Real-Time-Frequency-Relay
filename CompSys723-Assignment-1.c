/*
 * Assignment1.c
 *
 *  Created on: 9/04/2015
 *      Author: lols017
 */

//Includes
#include <system.h>
#include <altera_avalon_pio_regs.h>
#include <stdio.h>
#include <ctype.h>
#include <unistd.h>
#include "altera_up_avalon_ps2.h"
#include "altera_up_ps2_keyboard.h"
#include "alt_types.h"                 	// alt_u32 is a kind of alt_types
#include "sys/alt_irq.h"              	// to register interrupts
#include <stdlib.h>
#include "io.h"
#include "altera_up_avalon_video_character_buffer_with_dma.h"
#include "altera_up_avalon_video_pixel_buffer_dma.h"

// Scheduler includes
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"

//Frequency Plot Defines
#define FREQPLT_ORI_X 101		// x axis pixel position at the plot origin
#define FREQPLT_GRID_SIZE_X 5	// pixel separation in the x axis between two data points
#define FREQPLT_ORI_Y 199.0		// y axis pixel position at the plot origin
#define FREQPLT_FREQ_RES 20.0	// number of pixels per Hz (y axis scale)
#define ROCPLT_ORI_X 101
#define ROCPLT_GRID_SIZE_X 5
#define ROCPLT_ORI_Y 259.0
#define ROCPLT_ROC_RES 0.5		// number of pixels per Hz/s (y axis scale)
#define MIN_FREQ 45.0           // minimum frequency to draw

// Definition of Task Stacks
#define   TASK_STACKSIZE       2048

// Definition of Task Priorities
#define MAIN_TASK_PRIORITY 			14
#define SWITCHES_TASK_PRIORITY      13
#define KEYBOARD_TASK_PRIORITY		12
#define PRVGADraw_Task_P      (tskIDLE_PRIORITY+1)

typedef struct{
    unsigned int x1;
    unsigned int y1;
    unsigned int x2;
    unsigned int y2;
}Line;

//Handles
SemaphoreHandle_t shared_currentControlledLoad_sem;
static QueueHandle_t Q_freq_data;
static QueueHandle_t Q_keyboard_input;
TaskHandle_t PRVGADraw;
TimerHandle_t timer500ms;

// Global Variables

//Current loads switched on
unsigned int currentLoads = 0xFF;
//Controlled loads
unsigned int controlledLoads = 0x00;

//System stability indicator, 1 = stable, 0 = unstable
int systemStable = 1;

double minFreqThreshold = 50;
double maxRoCThreshold = 10;

//String indicating what keyboard input is changing
char changeString[25]= "Min Frequency Threshold";

//Determines if an invalid key should be skipped
int invalidKey = 0;

//Times taken to shed the load initially upon the system moving from stable to unstable
int timeArray[5] = {1,1,1,1,1};

//System time in milliseconds
int ticks = 0;

//Current number entered by the user
double currentNumber = 0;

double currentFreq = 0;

//FSM states
typedef enum {NORMAL, LOAD_MANAGING, MAINTENANCE} systemState;
systemState state = NORMAL;

// Local Function Prototypes
int initCreateTasks(void);
void freq_relay_isr(void);
void vTimerCallback( TimerHandle_t pxTimer );

// Keyboard Scan Code Lookup Table
#define TAB 0x09 // Tab
#define BKSP 0x08 // Backspace
#define ENTER 0x0d // Enter
#define ESC 0x1b // Escape
#define BKSL 0x5c // Backslash

const char strToAscii[128] =
{												//Leftmost value
    0, 0,   0,     0,   0,   0,    0,    0, //00
    0, 0,   0,     0,   0,   0,    0,    0, //08
    0, 0,   0,     0,   0,   'q',  '1',  0, //10
    0, 0,   'z',   's', 'a', 'w',  '2',  0, //18
    0, 'c', 'x',   'd', 'e', '4',  '3',  0, //20
    0, ' ', 'v',   'f', 't', 'r',  '5',  0, //28
    0, 'n', 'b',   'h', 'g', 'y',  '6',  0, //30
    0, 0,   'm',   'j', 'u', '7',  '8',  0, //38
    0, ',', 'k',   'i', 'o', '0',  '9',  0, //40
    0, '.', '/',   'l', ';', 'p',  '-',  0, //48
    0, 0,   '\'',  0,   '[', '=',  0,    0, //50
    0, 0,   ENTER, ']', 0,   BKSL, 0,    0, //58
    0, 0,   0,     0,   0,   0,    BKSP, 0, //60
    0, '1', 0,     '4', '7', 0,    0,    0, //68
    '0', '.', '2',   '5', '6', '8',  ESC,  0, //70
    0, '+', '3',   '-', '*', '9',  0,    0 //78
    
};

//ISR to take input keys from the keyboard and send valid bytes to the keyboard queue for decoding
void ps2_isr(void* ps2_device, alt_u32 id){
    
    unsigned char byte;
    unsigned char tempChar;
    
    //Read the keyboard input
    alt_up_ps2_read_data_byte_timeout(ps2_device, &byte);
    tempChar = byte;
    
    //Do not send invalid input bytes to the queue
    if(byte > 0x7F){
        if( (byte == 0xF0) || (byte == 0xE0) ){
            invalidKey = 1;
        }
        return;
    }
    
    //Skip the invalid bytes
    if(invalidKey){
        invalidKey = 0;
        return;
    }
    
    //Add valid key bytes to the keyboard queue to be decoded
    xQueueSendToBackFromISR( Q_keyboard_input, &tempChar, pdFALSE );
    return;
}

// The button ISR captures button presses on the key[3] button
// if the system is not load managing, it will progress to the maintenance state
void button_isr(void* context, alt_u32 id)
{
    // Cast the context before using it
    int* button = (int*) context;
    (*button) = IORD_ALTERA_AVALON_PIO_EDGE_CAP(PUSH_BUTTON_BASE);
    
    // Clears the edge capture register
    IOWR_ALTERA_AVALON_PIO_EDGE_CAP(PUSH_BUTTON_BASE, 0x07);
    
    // Enable interrupts for key[3] button
    IOWR_ALTERA_AVALON_PIO_IRQ_MASK(PUSH_BUTTON_BASE, 0x04);
    
    // Change the state to maintenance if the system is not load managing
    if ( (*button == 0x04) && (state == NORMAL)){
        state = MAINTENANCE;
    }
    else if( (*button == 0x04) && (state == MAINTENANCE)){
        state = NORMAL;
    }
    return;
}

// Updates the frequency data inputs to the system and determines the system's stability
void freq_relay_isr(){
#define SAMPLING_FREQ 16000.0
    // Read the new frequency value
    double temp = SAMPLING_FREQ/(double)IORD(FREQUENCY_ANALYSER_BASE, 0);
    // Update the rate of change
    double currentFreqRoC = ((temp - currentFreq)* 2.0 * temp * currentFreq ) / (temp + currentFreq);
    currentFreq = temp;
    
    // Ensure the rate of change is positive
    if(currentFreqRoC < 0){
        currentFreqRoC *= -1;
    }
    
    // If the current frequency is below the minimum threshold or current rate of change
    // of the frequency is larger than the maximum threshold, change the system stability to unstable
    if( (currentFreq < minFreqThreshold) || (currentFreqRoC >= maxRoCThreshold ) ){
        systemStable = 0;
        
        //Switch the state to load managing if the system is in the Normal operating state
        if(state == NORMAL){
            state = LOAD_MANAGING;
            //Obtain the current time to determine the time to shed the first load
            ticks = xTaskGetTickCountFromISR();
        }
    }
    else{
        systemStable = 1;
    }
    
    //Send the new frequency value to the frequency queue
    xQueueSendToBackFromISR( Q_freq_data, &temp, pdFALSE );
    return;
}


int main(int argc, char* argv[], char* envp[])
{
    
    int buttonValue = 0;
    
    // Clears the edge capture register. Writing 1 to bit clears pending interrupt for corresponding button.
    IOWR_ALTERA_AVALON_PIO_EDGE_CAP(PUSH_BUTTON_BASE, 0x07);
    
    // Enable interrupts for buttons
    IOWR_ALTERA_AVALON_PIO_IRQ_MASK(PUSH_BUTTON_BASE, 0x04);
    
    // Register the button ISR RICHARD ADDED THIS
    alt_irq_register(PUSH_BUTTON_IRQ,(void*)&buttonValue, button_isr);
    
    // Set up the PS2 Keyboard and ISR
    alt_up_ps2_dev * ps2_device = alt_up_ps2_open_dev(PS2_NAME);
    
    if(ps2_device == NULL){
        printf("can't find PS/2 device\n");
        return 1;
    }
    
    alt_up_ps2_enable_read_interrupt(ps2_device);
    alt_irq_register(PS2_IRQ, ps2_device, ps2_isr);
    IOWR_8DIRECT(PS2_BASE,4,1);
    
    // Create the queues
    Q_freq_data = xQueueCreate( 100, sizeof(double) );
    Q_keyboard_input = xQueueCreate(100, sizeof(char));
    
    // Register the frequency relay ISR
    alt_irq_register(FREQUENCY_ANALYSER_IRQ, 0, freq_relay_isr);
    
    // Create tasks and start the scheduler
    initCreateTasks();
    vTaskStartScheduler();
    
    for (;;);
    return 0;
}

/****** VGA display ******/

void PRVGADraw_Task(void *pvParameters ){
    
    //Initialize VGA controllers
    alt_up_pixel_buffer_dma_dev *pixel_buf;
    pixel_buf = alt_up_pixel_buffer_dma_open_dev(VIDEO_PIXEL_BUFFER_DMA_NAME);
    if(pixel_buf == NULL){
        printf("Can't find pixel buffer device\n");
    }
    alt_up_pixel_buffer_dma_clear_screen(pixel_buf, 0);
    
    alt_up_char_buffer_dev *char_buf;
    char_buf = alt_up_char_buffer_open_dev("/dev/video_character_buffer_with_dma");
    if(char_buf == NULL){
        printf("Can't find char buffer device\n");
    }
    alt_up_char_buffer_clear(char_buf);
    
    //Set up plot axes
    alt_up_pixel_buffer_dma_draw_hline(pixel_buf, 100, 590, 200, ((0x3ff << 20) + (0x3ff << 10) + (0x3ff)), 0);
    alt_up_pixel_buffer_dma_draw_hline(pixel_buf, 100, 590, 300, ((0x3ff << 20) + (0x3ff << 10) + (0x3ff)), 0);
    alt_up_pixel_buffer_dma_draw_vline(pixel_buf, 100, 50, 200, ((0x3ff << 20) + (0x3ff << 10) + (0x3ff)), 0);
    alt_up_pixel_buffer_dma_draw_vline(pixel_buf, 100, 220, 300, ((0x3ff << 20) + (0x3ff << 10) + (0x3ff)), 0);
    
    alt_up_char_buffer_string(char_buf, "Frequency(Hz)", 4, 4);
    alt_up_char_buffer_string(char_buf, "52", 10, 7);
    alt_up_char_buffer_string(char_buf, "50", 10, 12);
    alt_up_char_buffer_string(char_buf, "48", 10, 17);
    alt_up_char_buffer_string(char_buf, "46", 10, 22);
    
    alt_up_char_buffer_string(char_buf, "df/dt(Hz/s)", 4, 26);
    alt_up_char_buffer_string(char_buf, "60", 10, 28);
    alt_up_char_buffer_string(char_buf, "30", 10, 30);
    alt_up_char_buffer_string(char_buf, "0", 10, 32);
    alt_up_char_buffer_string(char_buf, "-30", 9, 34);
    alt_up_char_buffer_string(char_buf, "-60", 9, 36);
    
    // Initialise VGA variables
    char vgaOutputString[50];
    double freq[100], dfreq[100];
    int i = 99, j = 0;
    Line line_freq, line_roc;
    
    // System information variables
    float avgShedTime = 0;
    int maxShedTime = 0;
    int minShedTime = 1;
    float systemRunTime = 0;
    
    // Values to calculate the average time
    int timeValue = 0;
    float timeSum = 0;
    
    while(1){
        
        //Receive frequency data from queue
        while(uxQueueMessagesWaiting( Q_freq_data ) != 0){
            xQueueReceive( Q_freq_data, freq+i, 0 );
            
            //Calculate frequency RoC
            if(i==0){
                dfreq[0] = (freq[0]-freq[99]) * 2.0 * freq[0] * freq[99] / (freq[0]+freq[99]);
            }
            else{
                dfreq[i] = (freq[i]-freq[i-1]) * 2.0 * freq[i]* freq[i-1] / (freq[i]+freq[i-1]);
            }
            
            if (dfreq[i] > 100.0){
                dfreq[i] = 100.0;
            }
            
            i =	++i%100; //point to the next data (oldest) to be overwritten
        }
        
        //Clear old graph to draw new graph
        alt_up_pixel_buffer_dma_draw_box(pixel_buf, 101, 0, 639, 199, 0, 0);
        alt_up_pixel_buffer_dma_draw_box(pixel_buf, 101, 201, 639, 299, 0, 0);
        
        for(j=0;j<99;++j){ //i here points to the oldest data, j loops through all the data to be drawn on VGA
            if (((int)(freq[(i+j)%100]) > MIN_FREQ) && ((int)(freq[(i+j+1)%100]) > MIN_FREQ)){
                //Calculate coordinates of the two data points to draw a line in between
                //Frequency plot
                line_freq.x1 = FREQPLT_ORI_X + FREQPLT_GRID_SIZE_X * j;
                line_freq.y1 = (int)(FREQPLT_ORI_Y - FREQPLT_FREQ_RES * (freq[(i+j)%100] - MIN_FREQ));
                
                line_freq.x2 = FREQPLT_ORI_X + FREQPLT_GRID_SIZE_X * (j + 1);
                line_freq.y2 = (int)(FREQPLT_ORI_Y - FREQPLT_FREQ_RES * (freq[(i+j+1)%100] - MIN_FREQ));
                
                //Frequency RoC plot
                line_roc.x1 = ROCPLT_ORI_X + ROCPLT_GRID_SIZE_X * j;
                line_roc.y1 = (int)(ROCPLT_ORI_Y - ROCPLT_ROC_RES * dfreq[(i+j)%100]);
                
                line_roc.x2 = ROCPLT_ORI_X + ROCPLT_GRID_SIZE_X * (j + 1);
                line_roc.y2 = (int)(ROCPLT_ORI_Y - ROCPLT_ROC_RES * dfreq[(i+j+1)%100]);
                
                //Draw
                alt_up_pixel_buffer_dma_draw_line(pixel_buf, line_freq.x1, line_freq.y1, line_freq.x2, line_freq.y2, 0x3ff << 0, 0);
                alt_up_pixel_buffer_dma_draw_line(pixel_buf, line_roc.x1, line_roc.y1, line_roc.x2, line_roc.y2, 0x3ff << 0, 0);
            }
        }
        
        // Calculate the systems timing information
        // get the most recent 5 initial load shed times
        int timeArrayIndex = 0;
        for(timeArrayIndex = 0; timeArrayIndex < 5; timeArrayIndex++){
            
            timeValue = timeArray[timeArrayIndex];
            
            timeSum += timeValue;
            // Find the maximum load shed time
            if(timeValue > maxShedTime){
                maxShedTime = timeValue;
            }
            // Find the minimum load shed time
            else if(timeValue < minShedTime){
                minShedTime = timeValue;
            }
        }
        
        // Calculate the average for the current 5 timing variables
        timeSum = (timeSum / 5);
        
        //Round the minimum value up if it is 0
        if(minShedTime == 0){
            minShedTime = 1;
        }
        // Update the system average load shed time
        avgShedTime = timeSum;
        timeSum = 0;
        
        //Calculate the current system run time in seconds
        systemRunTime = xTaskGetTickCount();
        systemRunTime *= 0.001;
        
        // Display the relevant information to the VGA display
        
        // Current keyboard input to change the chosen threshold
        sprintf(vgaOutputString,"Changing %s to : %.3f        ", changeString,currentNumber);
        alt_up_char_buffer_string(char_buf, vgaOutputString, 4, 39);
        
        // Display the current system state
        switch(state){
            case NORMAL:
                sprintf(vgaOutputString,"Current State : Normal        ");
                break;
            case LOAD_MANAGING:
                sprintf(vgaOutputString,"Current State : Load Managing   ");
                break;
            case MAINTENANCE:
                sprintf(vgaOutputString,"Current State : Maintenance   ");
                break;
        }
        alt_up_char_buffer_string(char_buf, vgaOutputString, 4, 41);
        
        // Display the system thresholds and the corresponding titles
        sprintf(vgaOutputString,"Min Frequency Threshold");
        alt_up_char_buffer_string(char_buf, vgaOutputString, 4, 43);
        
        sprintf(vgaOutputString,"Max df/dt Threshold");
        alt_up_char_buffer_string(char_buf, vgaOutputString, 40, 43);
        
        sprintf(vgaOutputString,"Min Frequency (Hz): %.3f     ",minFreqThreshold);
        alt_up_char_buffer_string(char_buf, vgaOutputString, 4, 45);
        
        sprintf(vgaOutputString,"Max df/dt (Hz/s): %.3f       ",maxRoCThreshold);
        alt_up_char_buffer_string(char_buf, vgaOutputString , 40, 45);
        
        // Display the system timing and stability information in the left hand column
        sprintf(vgaOutputString,"System Information ");
        alt_up_char_buffer_string(char_buf, vgaOutputString , 7, 48);
        
        if(systemStable){
            alt_up_char_buffer_string(char_buf, "System Stability : Stable  " , 4, 50);
        }
        else{
            alt_up_char_buffer_string(char_buf, "System Stability : Unstable" , 4, 50);
        }
        
        sprintf(vgaOutputString,"Max shed time    : %d ms      ",maxShedTime);
        alt_up_char_buffer_string(char_buf, vgaOutputString , 4, 52);
        
        sprintf(vgaOutputString,"Min shed time    : %d ms        ",minShedTime);
        alt_up_char_buffer_string(char_buf, vgaOutputString , 4, 54);
        
        sprintf(vgaOutputString,"Avg shed time    : %.3f ms        ",avgShedTime);
        alt_up_char_buffer_string(char_buf, vgaOutputString , 4, 56);
        
        sprintf(vgaOutputString,"Run time         : %.3f s        ",systemRunTime);
        alt_up_char_buffer_string(char_buf, vgaOutputString , 4, 58);
        
        // Display the five most recent initial load shed times in the right hand column
        sprintf(vgaOutputString,"Recent Initial Load Shed Times :");
        alt_up_char_buffer_string(char_buf, vgaOutputString , 40, 48);
        
        sprintf(vgaOutputString," %d ms     ",timeArray[4]);
        alt_up_char_buffer_string(char_buf, vgaOutputString , 52, 50);
        
        sprintf(vgaOutputString," %d ms     ",timeArray[3]);
        alt_up_char_buffer_string(char_buf, vgaOutputString , 52, 52);
        
        sprintf(vgaOutputString," %d ms    ",timeArray[2]);
        alt_up_char_buffer_string(char_buf, vgaOutputString , 52, 54);
        
        sprintf(vgaOutputString," %d ms     ",timeArray[1]);
        alt_up_char_buffer_string(char_buf, vgaOutputString , 52, 56);
        
        sprintf(vgaOutputString," %d ms     ",timeArray[0]);
        alt_up_char_buffer_string(char_buf, vgaOutputString , 52, 58);
        
        // Delay the task for 10ms
        vTaskDelay(10);
    }
}

//Task to handle decoding of the keyboard input
void keyboard_task()
{
    // Keyboard variables
    unsigned char byte;
    int keyboardInputIndex = 0;
    char tempCharBuffer[100];
    int tempIndex = 0;
    
    // Keyboard input character array
    char keyboardInput[7];
    int charIndex = 0;
    
    // Initially fill the array with '.' to start the initial number read as 0.000
    int i=0;
    for(i=0;i<7;i++){
        keyboardInput[i] = '.';
    }
    
    while(1){
        
        // Read from the keyboard queue while it contains messages
        while(uxQueueMessagesWaiting( Q_keyboard_input ) != 0){
            
            //Buffer the input byte
            byte = tempCharBuffer[tempIndex];
            
            //R Key indicates the user wishes to change the maximum frequency rate of change threshold
            if(byte == 0x2D ){
                strcpy(changeString,"Max df/dt Threshold");
            }
            //F Key indicates the user wishes to change the minimum frequency threshold
            else if (byte == 0x2B){
                strcpy(changeString,"Min Frequency Threshold");
            }
            
            // If enter is pressed, evaluate the input and update the corresponding threshold
            if(byte == 0x5A){
                // Convert the keyboard string into a double
                char *ptr;
                double ret = strtod(keyboardInput, &ptr);
                
                // Clear the keyboard array
                for(keyboardInputIndex = 0; keyboardInputIndex < 7; keyboardInputIndex++){
                    keyboardInput[keyboardInputIndex] = '.';
                }
                
                // Determine which threshold needs updating, "i" for Min Frequency, "a" for Max Rate of change
                if(changeString[1] == 'i'){
                    minFreqThreshold = ret;//set with key, read
                }
                else if(changeString[1] == 'a'){
                    maxRoCThreshold = ret;//set with key, read
                }
                
                // Clear the current number and index variables
                charIndex = 0;
                currentNumber = 0;
                tempIndex = 0;
            }
            // If escape is pressed clear and exit
            else if(byte == 0x76){
                
                // Clear the keyboard array
                for(keyboardInputIndex = 0; keyboardInputIndex < 7; keyboardInputIndex++){
                    keyboardInput[keyboardInputIndex] = '.';
                }
                // Clear the current number and index variables
                charIndex = 0;
                currentNumber = 0;
                tempIndex = 0;
            }
            // Otherwise add the new character to the keyboard input array
            else{
                char newChar = strToAscii[byte];
                // Only add characters if the are digits or decimal points
                if( ( isdigit(newChar) ) || (byte ==0x49) || (byte ==0x71) ){
                    tempIndex++;
                    // Maximum of 6 input characters
                    if(charIndex < 6){
                        keyboardInput[charIndex] = newChar;
                        charIndex++;
                    }
                }
                
                // Update the current number displayed
                char *ptr;
                currentNumber = strtod(keyboardInput, &ptr);
            }
        }
        // Delay the task for 10ms
        vTaskDelay(10);
    }
    return;
}

// The switches task determines which loads the user has turned on
void switches_task ()
{
    while(1){
        
        unsigned int uiSwitchValue = 0;
        // Read the value of the switches and store to uiSwitchValue
        uiSwitchValue = IORD_ALTERA_AVALON_PIO_DATA(SLIDE_SWITCH_BASE);
        uiSwitchValue = (uiSwitchValue &  0x0FF);
        
        shared_currentControlledLoad_sem = xSemaphoreCreateMutex();
        
        // Checks semaphore has been created
        if (shared_currentControlledLoad_sem != NULL) {
            
            // Take the semaphore if possible with a timeout of 100ms
            if (xSemaphoreTake(shared_currentControlledLoad_sem, 100) == pdTRUE) {
                // If the system is load managing, only switch loads off
                if(state == LOAD_MANAGING){
                    // write the value of the switches to the red LEDs
                    currentLoads = uiSwitchValue & currentLoads;
                    IOWR_ALTERA_AVALON_PIO_DATA(RED_LEDS_BASE, currentLoads);
                }
                // Otherwise switch the loads as required
                else{
                    // Write the value of the switches to the red LEDs
                    currentLoads = uiSwitchValue;
                    IOWR_ALTERA_AVALON_PIO_DATA(RED_LEDS_BASE, uiSwitchValue);
                }
                // Return the semaphore
                xSemaphoreGive(shared_currentControlledLoad_sem);
            }
        }
        // Write the value of the switches to the seven segment display
        IOWR_ALTERA_AVALON_PIO_DATA(SEVEN_SEG_BASE, uiSwitchValue);
        
        // Delay the task for 100ms
        vTaskDelay(100);
    }
}

// This task runs the main system FSM
void main_task()
{
    int timerRunning = 0;
    int previousStable = 1;
    int ticksNew = 0;
    int timeIndex = 4;
    
    while(1){
        // FSM: Normal, Maintenance, Load managing
        switch(state){
                
                // System will switch loads as the user needs but will enter load managing state if the frequency drops below
                // the minimum threshold or the rate of change of frequency rises above the maximum threshold
            case NORMAL:
                // Clear the 500ms load managing timer flag
                timerRunning = 0;
                break;
                
                // System will not manage loads, only manual load switching will occur
            case MAINTENANCE:
                // Clear the 500ms load managing timer flag
                timerRunning = 0;
                break;
                
                // System manages the loads, switching off the lowest priority loads if the system is still unstable
                // or adding the highest priority loads if it is still stable after 500ms
            case LOAD_MANAGING:
                
                // Start the 500ms load timer if it is not running and initialse the load managing state
                if(!timerRunning){
                    
                    // Shed the first load and record the system response time
                    shedLoads();
                    ticksNew = xTaskGetTickCount();
                    ticksNew = (ticksNew - ticks);
                    
                    // Update the load shed times in the array and shift the values to the left as required
                    if(timeIndex == 4){
                        int tempp = 0;
                        for(tempp = 1; tempp < 5;tempp++){
                            timeArray[tempp-1] = timeArray[tempp];
                        }
                        timeArray[timeIndex] = ticksNew;
                    }
                    else{
                        timeArray[timeIndex] = ticksNew;
                        timeIndex ++;
                    }
                    
                    // Set the previousStable variable to the current system stability (unstable)
                    previousStable = systemStable;
                    // Start the timer
                    xTimerStart( timer500ms, 1 );
                    // Set the timer running flag to true
                    timerRunning = 1;
                }
                
                // If the system stability has changed, reset the timer
                if(systemStable != previousStable){
                    // Reset 500ms timer
                    xTimerReset( timer500ms,0 );
                }
                
                // Set the previous system stability variable
                previousStable = systemStable;
                break;
        }
        
        // Delay the task for 5ms
        vTaskDelay(5);
    }
    
}

// This function restores the highest priority load
void restoreLoads (){
    
    // Initialise the variables
    unsigned int shifter = 0x80; // highest load value
    unsigned int value = 0;
    unsigned int loadOn = 0;
    unsigned int uSwitchValue = 0;
    
    // Create the semaphore
    shared_currentControlledLoad_sem = xSemaphoreCreateMutex();
    
    // Check if the semaphore was created correctly
    if (shared_currentControlledLoad_sem != NULL) {
        
        // Take the semaphore if possible with a timeout of 100ms
        if (xSemaphoreTake(shared_currentControlledLoad_sem, 100) == pdTRUE) {
            
            // While the shifter has not finished iterating over the controlled loads and
            // there are currently controlled loads
            while( (shifter != 0x00) && (controlledLoads != 0) ){
                
                // Determine if the load is controlled
                value = (controlledLoads & shifter);
                // Determine if the load is still on
                uSwitchValue = IORD_ALTERA_AVALON_PIO_DATA(SLIDE_SWITCH_BASE);
                loadOn = (uSwitchValue & shifter);
                
                // If a load is controlled
                if( (value > 0) ){
                    
                    // Release control of the load
                    controlledLoads = (controlledLoads ^ shifter);
                    // If the load is still switched on, turn it back on and
                    // update the current loads
                    if(loadOn){
                        currentLoads |= shifter;
                        // Exit as a load has been switched
                        shifter = 0x00;
                        // Return to the normal state if all controlled loads have been released
                        if(controlledLoads == 0x00){
                            state = NORMAL;
                            // Stop the timer
                            xTimerStop(timer500ms, 1);
                        }
                    }
                }
                // Otherwise move the shifter to check the next controlled load
                else{
                    shifter = (shifter >> 1);
                }
            }
            // Return to the normal state if all controlled loads have been released
            if(controlledLoads == 0){
                state = NORMAL;
                // Stop the timer
                xTimerStop(timer500ms, 1);
            }
            // Update the LED displays and return control of the semaphore
            IOWR_ALTERA_AVALON_PIO_DATA(RED_LEDS_BASE, currentLoads);
            IOWR_ALTERA_AVALON_PIO_DATA(GREEN_LEDS_BASE, controlledLoads);
            xSemaphoreGive(shared_currentControlledLoad_sem);
        }
    }
}

// This function is used to shed the lowest priority load
void shedLoads(){
    
    unsigned int shifter = 0x01;// lowest load value
    unsigned int value = 0;
    
    // Create the semaphore
    shared_currentControlledLoad_sem = xSemaphoreCreateMutex();
    
    // Check the semaphore was created correctly
    if (shared_currentControlledLoad_sem != NULL) {
        
        // Take control of the semaphore if possible, with a 100ms timeout
        if (xSemaphoreTake(shared_currentControlledLoad_sem, 100) == pdTRUE) {
            
            // While the shifter is less than the highest priority load position and there are loads to shed
            while( (shifter < 0x100) && (currentLoads != 0) ){
                
                // Determine if the load is switched on
                value = (currentLoads & shifter);
                // If the load is switched on, shed it and update the controlled loads
                if(value > 0){
                    currentLoads = ( currentLoads ^ shifter );
                    controlledLoads = (controlledLoads | shifter );
                    shifter = 0x100; // Update the shifter to exit the while loop
                }
                // Otherwise move the shifter to analyse the next load
                else{
                    shifter = (shifter << 1);
                }
            }
            
            // Update the LED displays and release the semaphore
            IOWR_ALTERA_AVALON_PIO_DATA(RED_LEDS_BASE, currentLoads);
            IOWR_ALTERA_AVALON_PIO_DATA(GREEN_LEDS_BASE, controlledLoads);
            xSemaphoreGive(shared_currentControlledLoad_sem);
        }
    }
}

// Timer callback function to restore or shed loads if the system has remained stable / unstable for 500ms
void vTimerCallback( TimerHandle_t pxTimer ){
    
    // If the timer initiating the callback is the 500ms load timer
    if(pxTimer == timer500ms){
        
        // If the system has remained stable restore a load
        if(systemStable){
            restoreLoads();
        }
        // Shed a load if the system is still unstable
        else if (!systemStable){
            shedLoads();
        }
    }
}

// This function creates the tasks used in this frequency relay system
int initCreateTasks(void)
{
    // 500ms load shed timer
    timer500ms = xTimerCreate( "500ms Timer",( 500 / portTICK_PERIOD_MS ), pdTRUE, (void *) 500, vTimerCallback);
    // Switches task
    xTaskCreate(switches_task, "switches_task", TASK_STACKSIZE, NULL, SWITCHES_TASK_PRIORITY, NULL);
    // Keyboard task
    xTaskCreate(keyboard_task, "keyboard_task", TASK_STACKSIZE, NULL, KEYBOARD_TASK_PRIORITY, NULL);
    // VGA display task
    xTaskCreate( PRVGADraw_Task, "DrawTsk", configMINIMAL_STACK_SIZE, NULL, PRVGADraw_Task_P, &PRVGADraw );
    // Main FSM task
    xTaskCreate(main_task, "main_task", TASK_STACKSIZE, NULL, MAIN_TASK_PRIORITY, NULL);
    
    return 0;
}
