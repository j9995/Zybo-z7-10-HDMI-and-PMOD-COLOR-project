/************************************************************************/
/*																		*/
/*	video_demo.c	--	ZYBO Video demonstration 						*/
/*																		*/
/************************************************************************/
/*	Author: Jesse Jimenez | Sam Bobrowicz								*/
/*	Copyright 2015, Digilent Inc.										*/
/************************************************************************/
/*  Module Description: 												*/
/*																		*/
/*		This file contains code for running a demonstration of the		*/
/*		Video input and output capabilities on the ZYBO. It is a good	*/
/*		example of how to properly use the display_ctrl and				*/
/*		video_capture drivers.											*/
/*																		*/
/*																		*/
/************************************************************************/
/*  Revision History:													*/
/* 																		*/
/*		5/5/2021: Created										*/
/*																		*/
/************************************************************************/

/* ------------------------------------------------------------ */
/*				Include File Definitions						*/
/* ------------------------------------------------------------ */
#include "PmodCOLOR.h"
#include "sleep.h"
#include "video_demo.h"
#include "display_ctrl/display_ctrl.h"
#include "intc/intc.h"
#include <stdio.h>
#include "xuartps.h"
#include "math.h"
#include <ctype.h>
#include <stdlib.h>
#include "xil_types.h"
#include "xil_cache.h"
#include "timer_ps/timer_ps.h"
#include "xparameters.h"
#include "xgpio.h"

/*
 * XPAR redefines
 */
#define DYNCLK_BASEADDR 		XPAR_AXI_DYNCLK_0_BASEADDR
#define VDMA_ID 				XPAR_AXIVDMA_0_DEVICE_ID
#define HDMI_OUT_VTC_ID 		XPAR_V_TC_OUT_DEVICE_ID
#define SCU_TIMER_ID 			XPAR_SCUTIMER_DEVICE_ID
#define UART_BASEADDR 			XPAR_PS7_UART_1_BASEADDR

/*
* Struct for Pmod
*/
typedef struct {
   COLOR_Data min, max;
} CalibrationData;


/* ------------------------------------------------------------ */
/*				Global Variables								*/
/* ------------------------------------------------------------ */

/*
 * Display and Video Driver structs
 */
DisplayCtrl dispCtrl;
XAxiVdma vdma;
INTC intc;
char fRefresh; //flag used to trigger a refresh of the Menu on video detect

/*
 * Framebuffers for video data
 */
u8 frameBuf[DISPLAY_NUM_FRAMES][DEMO_MAX_FRAME] __attribute__((aligned(0x20)));
u8 *pFrames[DISPLAY_NUM_FRAMES]; //array of pointers to the frame buffers

/*
* Functions for Pmod
*/
CalibrationData DemoInitCalibrationData(COLOR_Data firstSample);
void DemoInitialize2();
void DemoCalibrate(COLOR_Data newSample, CalibrationData *calib);
COLOR_Data DemoNormalizeToCalibration(COLOR_Data sample, CalibrationData calib);
void EnableCaches();
void DisableCaches();

PmodCOLOR myDevice;

/* ------------------------------------------------------------ */
/*				Procedure Definitions							*/
/* ------------------------------------------------------------ */

int main(void)
{
	DemoInitialize();

	DemoInitialize2();

	DemoRun();

	DisableCaches();

	return 0;
}


void DemoInitialize()
{
	int Status;
	XAxiVdma_Config *vdmaConfig;
	int i;

	/*
	 * Initialize an array of pointers to the 3 frame buffers
	 */
	for (i = 0; i < DISPLAY_NUM_FRAMES; i++)
	{
		pFrames[i] = frameBuf[i];
	}

	/*
	 * Initialize a timer used for a simple delay
	 */
	TimerInitialize(SCU_TIMER_ID);

	/*
	 * Initialize VDMA driver
	 */
	vdmaConfig = XAxiVdma_LookupConfig(VDMA_ID);
	if (!vdmaConfig)
	{
		xil_printf("No video DMA found for ID %d\r\n", VDMA_ID);
		return;
	}
	Status = XAxiVdma_CfgInitialize(&vdma, vdmaConfig, vdmaConfig->BaseAddress);
	if (Status != XST_SUCCESS)
	{
		xil_printf("VDMA Configuration Initialization failed %d\r\n", Status);
		return;
	}

	/*
	 * Initialize the Display controller and start it
	 */
	Status = DisplayInitialize(&dispCtrl, &vdma, HDMI_OUT_VTC_ID, DYNCLK_BASEADDR, pFrames, DEMO_STRIDE);
	if (Status != XST_SUCCESS)
	{
		xil_printf("Display Ctrl initialization failed during demo initialization%d\r\n", Status);
		return;
	}
	Status = DisplayStart(&dispCtrl);
	if (Status != XST_SUCCESS)
	{
		xil_printf("Couldn't start display during demo initialization%d\r\n", Status);
		return;
	}

	/*
	 * Initialize the Interrupt controller and start it.
	 */
	Status = fnInitInterruptController(&intc);
	if(Status != XST_SUCCESS) {
		xil_printf("Error initializing interrupts");
		return;
	}

	return;
}

void DemoInitialize2() {
   EnableCaches();
   COLOR_Begin(&myDevice, XPAR_PMODCOLOR_0_AXI_LITE_IIC_BASEADDR,
         XPAR_PMODCOLOR_0_AXI_LITE_GPIO_BASEADDR, 0x29);

   COLOR_SetENABLE(&myDevice, COLOR_REG_ENABLE_PON_MASK);
   usleep(2400);
   COLOR_SetENABLE(&myDevice,
         COLOR_REG_ENABLE_PON_MASK | COLOR_REG_ENABLE_RGBC_INIT_MASK);
   usleep(2400);
}

void DemoRun()
{
	XGpio dip, push; // Variables to access GPIO instance
	int psb_check, dip_check;
	int counter, counterq = 0;
	int nextFrame = 0;
	char userInput = 0;
	u8 ID;
	COLOR_Data data;
	CalibrationData calib;

	xil_printf("Pmod COLOR Demo launched\r\n");
	ID = COLOR_GetID(&myDevice);
	xil_printf("Device ID = %02x\r\n", ID);

	data = COLOR_GetData(&myDevice);
	calib = DemoInitCalibrationData(data);
	usleep(2400);

	//GPIO Set Up
	// Driver functions
	XGpio_Initialize(&dip, XPAR_SWITCHES_DEVICE_ID);
	// Set direction: input/output = '1111' or f refers to all four bits are input
	XGpio_SetDataDirection(&dip, 1, 0xffffffff);
	// (pointer to XGPIO data type, specified instance of GPIO) -- ID found in xparameter
	XGpio_Initialize(&push, XPAR_BUTTONS_DEVICE_ID);
	XGpio_SetDataDirection(&push, 1, 0xffffffff);

	// Start with white screen (update later)
	DemoPrintTest(pFrames[dispCtrl.curFrame], dispCtrl.vMode.width, dispCtrl.vMode.height, DEMO_STRIDE, DEMO_PATTERN_0);


	/* Flush UART FIFO */
	while (XUartPs_IsReceiveData(UART_BASEADDR))
	{
		XUartPs_ReadReg(UART_BASEADDR, XUARTPS_FIFO_OFFSET);
	}


	while (userInput != 'q')
	{
		// Read in dip-switch value
		// If dip-switch 1 is up, switch to color detecting mode
		dip_check = XGpio_DiscreteRead(&dip, 1);
		TimerDelay(550000); // debounce
		// If dip == 1, perform a color read
		if (dip_check == 1) 
		{
			xil_printf("You are now in color dectecting mode | DIP Switch Status %x\r\n", dip_check);
			while (dip_check == 1) 
			{
				data = COLOR_GetData(&myDevice);
				DemoCalibrate(data, &calib);
				data = DemoNormalizeToCalibration(data, calib);
				xil_printf("r=%04x g=%04x b=%04x\n\r", data.r, data.g, data.b);
				TimerDelay(550000); // debounce
				dip_check = XGpio_DiscreteRead(&dip, 1);
				
				// Place RGB LED control here if time applicable 
			}
		}
		else
		{
			fRefresh = 0;
			DemoPrintMenu();
			
			// Get input or wait for input
			psb_check = XGpio_DiscreteRead(&push, 1);
			//xil_printf("Current Push Buttons Status %x\r\n", psb_check);
			//while (psb_check == 0)
			//{
				//psb_check = XGpio_DiscreteRead(&push, 1);
				//TimerDelay(1000); // Extra Debounce needed if two or more buttons are pushed simultaneously
				//psb_check = XGpio_DiscreteRead(&push, 1);
			//}
			//xil_printf("New Push Buttons Status %x\r\n", psb_check);
			//TimerDelay(550000); // Debounce

			/* Wait for data on UART */
			//while (!XUartPs_IsReceiveData(UART_BASEADDR) && !fRefresh)
				//{}
			
			/* Wait for data on UART OR push button */
			while (!XUartPs_IsReceiveData(UART_BASEADDR) && !fRefresh && (psb_check == 0))
				{
					psb_check = XGpio_DiscreteRead(&push, 1);
				}
			xil_printf("New Push Buttons Status %x\r\n", psb_check);
			TimerDelay(50000); // Debounce

			// Store the first character in the UART receive FIFO and echo it
			if (XUartPs_IsReceiveData(UART_BASEADDR))
			{
				userInput = XUartPs_ReadReg(UART_BASEADDR, XUARTPS_FIFO_OFFSET);
				xil_printf("%c", userInput);
			}
		/*
		else  //Refresh triggered by video detect interrupt
		{
			userInput = 'r';
		}
		*/
			/* Scroll right if a 1 is entered */
			else if (psb_check == 1)
			{
				counter = counter + 1;
				if (counter == 1){
					userInput = '1';
				}
				else if (counter == 2){
					userInput = '2';
				}
				//else if (counter == 3){
					//userInput = '3';
				//}
				else if (counter > 2){
					userInput = '1';
					counter = 1;
				}
				else {
					counter = counter;
				}
			}
			/* Scroll left if a 2 is entered */
			else if (psb_check == 2)
			{
				counter = counter - 1;
				//if (counter == 3){
					//userInput = '3';
				//}
				if (counter == 2){
					userInput = '2';
				}
				else if (counter == 1){
					userInput = '1';
				}
				else if (counter == 0){
					userInput = '2';
					counter = 2;
				}
				else{
					counter = counter;
				}
			}
			/* Change Display Framebuffer Index if a 3 is entered */
			else if (psb_check == 3)
			{
				userInput = 'A';
			}
			/* Change res if a 4 is entered */
			else if (psb_check == 4)
			{
				userInput = '4';
			}
			else
			{
				userInput = 'r';
			}

			switch (userInput)
			{
			case '1':
				DemoPrintTest(pFrames[dispCtrl.curFrame], dispCtrl.vMode.width, dispCtrl.vMode.height, DEMO_STRIDE, DEMO_PATTERN_0);
				break;
			case '2':
				DemoPrintTest(pFrames[dispCtrl.curFrame], dispCtrl.vMode.width, dispCtrl.vMode.height, DEMO_STRIDE, DEMO_PATTERN_1);
				break;
		//case '3':
			//DemoPrintTest(pFrames[dispCtrl.curFrame], dispCtrl.vMode.width, dispCtrl.vMode.height, DEMO_STRIDE, DEMO_PATTERN_2);
			//break;
			case '4':
				DemoChangeRes();
				break;
			case 'A':
				nextFrame = dispCtrl.curFrame + 1;
				if (nextFrame >= DISPLAY_NUM_FRAMES)
				{
					nextFrame = 0;
				}
				DisplayChangeFrame(&dispCtrl, nextFrame);
				break;
			case 'q':
				xil_printf("Now quitting program");
				TimerDelay(550000);
				break;
			case 'r':
				xil_printf("\n\rInvalid Selection");
				TimerDelay(550000);
				break;
			default :
				xil_printf("\n\rInvalid Selection");
				TimerDelay(550000);
			}
		}
	}

	return;
}

void DemoPrintMenu()
{
	xil_printf("\x1B[H"); //Set cursor to top left of terminal
	xil_printf("\x1B[2J"); //Clear terminal
	xil_printf("**************************************************\n\r");
	xil_printf("*                ZYBO Video Demo                 *\n\r");
	xil_printf("**************************************************\n\r");
	xil_printf("*Display Resolution: %28s*\n\r", dispCtrl.vMode.label);
	printf("*Display Pixel Clock Freq. (MHz): %15.3f*\n\r", dispCtrl.pxlFreq);
	xil_printf("*Display Frame Index: %27d*\n\r", dispCtrl.curFrame);
	xil_printf("**************************************************\n\r");
	xil_printf("\n\r");
	xil_printf("Press 1 or 2 to cycle between the test patterns\n\r");
	xil_printf("Move Dip Switch One to the ON position to detect color on the next run\n\r");
	xil_printf("or press the corresponding push button value to select the option\n\r");
	xil_printf("1 - Print Blended Test Pattern to Display Framebuffer\n\r");
	xil_printf("2 - Print Color Bar Test Pattern to Display Framebuffer\n\r");
	xil_printf("3 - Change Display Framebuffer Index\n\r");
	xil_printf("4 - Change Display Resolution\n\r");
	xil_printf("9 - Quit\n\r");
	xil_printf("\n\r");
	xil_printf("\n\r");
	xil_printf("Enter a selection:");
}

void DemoChangeRes()
{
	int fResSet = 0;
	int status;
	char userInput = 0;

	/* Flush UART FIFO */
	while (XUartPs_IsReceiveData(UART_BASEADDR))
	{
		XUartPs_ReadReg(UART_BASEADDR, XUARTPS_FIFO_OFFSET);
	}

	while (!fResSet)
	{
		DemoCRMenu();

		/* Wait for data on UART */
		while (!XUartPs_IsReceiveData(UART_BASEADDR))
		{}

		/* Store the first character in the UART recieve FIFO and echo it */
		userInput = XUartPs_ReadReg(UART_BASEADDR, XUARTPS_FIFO_OFFSET);
		xil_printf("%c", userInput);
		status = XST_SUCCESS;
		switch (userInput)
		{
		case '1':
			status = DisplayStop(&dispCtrl);
			DisplaySetMode(&dispCtrl, &VMODE_640x480);
			DisplayStart(&dispCtrl);
			fResSet = 1;
			break;
		case '2':
			status = DisplayStop(&dispCtrl);
			DisplaySetMode(&dispCtrl, &VMODE_800x600);
			DisplayStart(&dispCtrl);
			fResSet = 1;
			break;
		case '3':
			status = DisplayStop(&dispCtrl);
			DisplaySetMode(&dispCtrl, &VMODE_1280x720);
			DisplayStart(&dispCtrl);
			fResSet = 1;
			break;
		case '4':
			status = DisplayStop(&dispCtrl);
			DisplaySetMode(&dispCtrl, &VMODE_1280x1024);
			DisplayStart(&dispCtrl);
			fResSet = 1;
			break;
		case '5':
			status = DisplayStop(&dispCtrl);
			DisplaySetMode(&dispCtrl, &VMODE_1600x900);
			DisplayStart(&dispCtrl);
			fResSet = 1;
			break;
		case '6':
			status = DisplayStop(&dispCtrl);
			DisplaySetMode(&dispCtrl, &VMODE_1920x1080);
			DisplayStart(&dispCtrl);
			fResSet = 1;
			break;
		case 'q':
			fResSet = 1;
			break;
		default :
			xil_printf("\n\rInvalid Selection");
			TimerDelay(500000);
		}
		if (status == XST_DMA_ERROR)
		{
			xil_printf("\n\rWARNING: AXI VDMA Error detected and cleared\n\r");
		}
	}
}

void DemoCRMenu()
{
	xil_printf("\x1B[H"); //Set cursor to top left of terminal
	xil_printf("\x1B[2J"); //Clear terminal
	xil_printf("**************************************************\n\r");
	xil_printf("*                ZYBO Video Demo                 *\n\r");
	xil_printf("**************************************************\n\r");
	xil_printf("*Current Resolution: %28s*\n\r", dispCtrl.vMode.label);
	printf("*Pixel Clock Freq. (MHz): %23.3f*\n\r", dispCtrl.pxlFreq);
	xil_printf("**************************************************\n\r");
	xil_printf("\n\r");
	xil_printf("1 - %s\n\r", VMODE_640x480.label);
	xil_printf("2 - %s\n\r", VMODE_800x600.label);
	xil_printf("3 - %s\n\r", VMODE_1280x720.label);
	xil_printf("4 - %s\n\r", VMODE_1280x1024.label);
	xil_printf("5 - %s\n\r", VMODE_1600x900.label);
	xil_printf("6 - %s\n\r", VMODE_1920x1080.label);
	xil_printf("q - Quit (don't change resolution)\n\r");
	xil_printf("\n\r");
	xil_printf("Select a new resolution:");
}

void DemoInvertFrame(u8 *srcFrame, u8 *destFrame, u32 width, u32 height, u32 stride)
{
	u32 xcoi, ycoi;
	u32 lineStart = 0;
	for(ycoi = 0; ycoi < height; ycoi++)
	{
		for(xcoi = 0; xcoi < (width * 3); xcoi+=3)
		{
			destFrame[xcoi + lineStart] = ~srcFrame[xcoi + lineStart];         //Red
			destFrame[xcoi + lineStart + 1] = ~srcFrame[xcoi + lineStart + 1]; //Blue
			destFrame[xcoi + lineStart + 2] = ~srcFrame[xcoi + lineStart + 2]; //Green
		}
		lineStart += stride;
	}
	/*
	 * Flush the framebuffer memory range to ensure changes are written to the
	 * actual memory, and therefore accessible by the VDMA.
	 */
	Xil_DCacheFlushRange((unsigned int) destFrame, DEMO_MAX_FRAME);
}


/*
 * Bilinear interpolation algorithm. Assumes both frames have the same stride.
 */
void DemoScaleFrame(u8 *srcFrame, u8 *destFrame, u32 srcWidth, u32 srcHeight, u32 destWidth, u32 destHeight, u32 stride)
{
	float xInc, yInc; // Width/height of a destination frame pixel in the source frame coordinate system
	float xcoSrc, ycoSrc; // Location of the destination pixel being operated on in the source frame coordinate system
	float x1y1, x2y1, x1y2, x2y2; //Used to store the color data of the four nearest source pixels to the destination pixel
	int ix1y1, ix2y1, ix1y2, ix2y2; //indexes into the source frame for the four nearest source pixels to the destination pixel
	float xDist, yDist; //distances between destination pixel and x1y1 source pixels in source frame coordinate system

	int xcoDest, ycoDest; // Location of the destination pixel being operated on in the destination coordinate system
	int iy1; //Used to store the index of the first source pixel in the line with y1
	int iDest; //index of the pixel data in the destination frame being operated on

	int i;

	xInc = ((float) srcWidth - 1.0) / ((float) destWidth);
	yInc = ((float) srcHeight - 1.0) / ((float) destHeight);

	ycoSrc = 0.0;
	for (ycoDest = 0; ycoDest < destHeight; ycoDest++)
	{
		iy1 = ((int) ycoSrc) * stride;
		yDist = ycoSrc - ((float) ((int) ycoSrc));

		/*
		 * Save some cycles in the loop below by presetting the destination
		 * index to the first pixel in the current line
		 */
		iDest = ycoDest * stride;

		xcoSrc = 0.0;
		for (xcoDest = 0; xcoDest < destWidth; xcoDest++)
		{
			ix1y1 = iy1 + ((int) xcoSrc) * 3;
			ix2y1 = ix1y1 + 3;
			ix1y2 = ix1y1 + stride;
			ix2y2 = ix1y1 + stride + 3;

			xDist = xcoSrc - ((float) ((int) xcoSrc));

			/*
			 * For loop handles all three colors
			 */
			for (i = 0; i < 3; i++)
			{
				x1y1 = (float) srcFrame[ix1y1 + i];
				x2y1 = (float) srcFrame[ix2y1 + i];
				x1y2 = (float) srcFrame[ix1y2 + i];
				x2y2 = (float) srcFrame[ix2y2 + i];

				/*
				 * Bilinear interpolation function
				 */
				destFrame[iDest] = (u8) ((1.0-yDist)*((1.0-xDist)*x1y1+xDist*x2y1) + yDist*((1.0-xDist)*x1y2+xDist*x2y2));
				iDest++;
			}
			xcoSrc += xInc;
		}
		ycoSrc += yInc;
	}

	/*
	 * Flush the framebuffer memory range to ensure changes are written to the
	 * actual memory, and therefore accessible by the VDMA.
	 */
	Xil_DCacheFlushRange((unsigned int) destFrame, DEMO_MAX_FRAME);

	return;
}

void DemoPrintTest(u8 *frame, u32 width, u32 height, u32 stride, int pattern)
{
	u32 xcoi, ycoi;
	u32 iPixelAddr;
	u8 wRed, wBlue, wGreen;
	u32 wCurrentInt;
	double fRed, fBlue, fGreen, fColor;
	u32 xLeft, xMid, xRight, xInt;
	u32 yMid, yInt;
	double xInc, yInc;


	switch (pattern)
	{
	case DEMO_PATTERN_0:

		xInt = width / 4; //Four intervals, each with width/4 pixels
		xLeft = xInt * 3;
		xMid = xInt * 2 * 3;
		xRight = xInt * 3 * 3;
		xInc = 256.0 / ((double) xInt); //256 color intensities are cycled through per interval (overflow must be caught when color=256.0)

		yInt = height / 2; //Two intervals, each with width/2 lines
		yMid = yInt;
		yInc = 256.0 / ((double) yInt); //256 color intensities are cycled through per interval (overflow must be caught when color=256.0)

		fBlue = 0.0;
		fRed = 256.0;
		for(xcoi = 0; xcoi < (width*3); xcoi+=3)
		{
			/*
			 * Convert color intensities to integers < 256, and trim values >=256
			 */
			wRed = (fRed >= 256.0) ? 255 : ((u8) fRed);
			wBlue = (fBlue >= 256.0) ? 255 : ((u8) fBlue);
			iPixelAddr = xcoi;
			fGreen = 0.0;
			for(ycoi = 0; ycoi < height; ycoi++)
			{

				wGreen = (fGreen >= 256.0) ? 255 : ((u8) fGreen);
				frame[iPixelAddr] = wRed;
				frame[iPixelAddr + 1] = wBlue;
				frame[iPixelAddr + 2] = wGreen;
				if (ycoi < yMid)
				{
					fGreen += yInc;
				}
				else
				{
					fGreen -= yInc;
				}

				/*
				 * This pattern is printed one vertical line at a time, so the address must be incremented
				 * by the stride instead of just 1.
				 */
				iPixelAddr += stride;
			}

			if (xcoi < xLeft)
			{
				fBlue = 0.0;
				fRed -= xInc;
			}
			else if (xcoi < xMid)
			{
				fBlue += xInc;
				fRed += xInc;
			}
			else if (xcoi < xRight)
			{
				fBlue -= xInc;
				fRed -= xInc;
			}
			else
			{
				fBlue += xInc;
				fRed = 0;
			}
		}
		/*
		 * Flush the framebuffer memory range to ensure changes are written to the
		 * actual memory, and therefore accessible by the VDMA.
		 */
		Xil_DCacheFlushRange((unsigned int) frame, DEMO_MAX_FRAME);
		break;
	case DEMO_PATTERN_1:

		xInt = width / 7; //Seven intervals, each with width/7 pixels
		xInc = 256.0 / ((double) xInt); //256 color intensities per interval. Notice that overflow is handled for this pattern.

		fColor = 0.0;
		wCurrentInt = 1;
		for(xcoi = 0; xcoi < (width*3); xcoi+=3)
		{

			/*
			 * Just draw white in the last partial interval (when width is not divisible by 7)
			 */
			if (wCurrentInt > 7)
			{
				wRed = 255;
				wBlue = 255;
				wGreen = 255;
			}
			else
			{
				if (wCurrentInt & 0b001)
					wRed = (u8) fColor;
				else
					wRed = 0;

				if (wCurrentInt & 0b010)
					wBlue = (u8) fColor;
				else
					wBlue = 0;

				if (wCurrentInt & 0b100)
					wGreen = (u8) fColor;
				else
					wGreen = 0;
			}

			iPixelAddr = xcoi;

			for(ycoi = 0; ycoi < height; ycoi++)
			{
				frame[iPixelAddr] = wRed;
				frame[iPixelAddr + 1] = wBlue;
				frame[iPixelAddr + 2] = wGreen;
				/*
				 * This pattern is printed one vertical line at a time, so the address must be incremented
				 * by the stride instead of just 1.
				 */
				iPixelAddr += stride;
			}

			fColor += xInc;
			if (fColor >= 256.0)
			{
				fColor = 0.0;
				wCurrentInt++;
			}
		}
		/*
		 * Flush the framebuffer memory range to ensure changes are written to the
		 * actual memory, and therefore accessible by the VDMA.
		 */
		Xil_DCacheFlushRange((unsigned int) frame, DEMO_MAX_FRAME);
		break;
	default :
		xil_printf("Error: invalid pattern passed to DemoPrintTest");
	}

}

void DemoISR(void *callBackRef, void *pVideo)
{
	char *data = (char *) callBackRef;
	*data = 1; //set fRefresh to 1
}

CalibrationData DemoInitCalibrationData(COLOR_Data firstSample) {
   CalibrationData calib;
   calib.min = firstSample;
   calib.max = firstSample;
   return calib;
}

void DemoCalibrate(COLOR_Data newSample, CalibrationData *calib) {
   if (newSample.c < calib->min.c) calib->min.c = newSample.c;
   if (newSample.r < calib->min.r) calib->min.r = newSample.r;
   if (newSample.g < calib->min.g) calib->min.g = newSample.g;
   if (newSample.b < calib->min.b) calib->min.b = newSample.b;

   if (newSample.c > calib->max.c) calib->max.c = newSample.c;
   if (newSample.r > calib->max.r) calib->max.r = newSample.r;
   if (newSample.g > calib->max.g) calib->max.g = newSample.g;
   if (newSample.b > calib->max.b) calib->max.b = newSample.b;
}

COLOR_Data DemoNormalizeToCalibration(COLOR_Data sample,
      CalibrationData calib) {
   COLOR_Data norm;
   norm.c = (sample.c - calib.min.c) * (0xFFFF / (calib.max.c - calib.min.c));
   norm.r = (sample.r - calib.min.r) * (0xFFFF / (calib.max.r - calib.min.r));
   norm.g = (sample.g - calib.min.g) * (0xFFFF / (calib.max.g - calib.min.g));
   norm.b = (sample.b - calib.min.b) * (0xFFFF / (calib.max.b - calib.min.b));
   return norm;
}

void EnableCaches() {
#ifdef __MICROBLAZE__
#ifdef XPAR_MICROBLAZE_USE_ICACHE
   Xil_ICacheEnable();
#endif
#ifdef XPAR_MICROBLAZE_USE_DCACHE
   Xil_DCacheEnable();
#endif
#endif
}

void DisableCaches() {
#ifdef __MICROBLAZE__
#ifdef XPAR_MICROBLAZE_USE_DCACHE
   Xil_DCacheDisable();
#endif
#ifdef XPAR_MICROBLAZE_USE_ICACHE
   Xil_ICacheDisable();
#endif
#endif
}
