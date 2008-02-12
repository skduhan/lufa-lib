/*
             MyUSB Library
     Copyright (C) Dean Camera, 2008.
              
  dean [at] fourwalledcubicle [dot] com
      www.fourwalledcubicle.com

 Released under the LGPL Licence, Version 3
*/

/*
	MyUSB USB Bootloader. This bootloader enumerates to the host
	as a DFU Class device, allowing for DFU-compatible programming
	software to load firmware onto the AVR.
	
	This bootloader is compatible with Atmel's FLIP application.
	However, it requires the use of Atmel's DFU drivers. You will
	need to install Atmel's FDU drivers prior to using this bootloader.
	
	As an open-source option, this bootloader is also compatible
	with the Linux Atmel USB DFU Programmer software, avaliable
	for download at http://sourceforge.net/projects/dfu-programmer/.
	
    If SECURE_MODE is defined, upon startup the bootloader will be
    locked, with only the chip erase function avaliable (similar to
    Atmel's DFU bootloader). If SECURE_MODE is undefined, all functions
    are usable on startup without the prerequisite firmware erase.
    
	*** WORK IN PROGRESS - NOT CURRENTLY FUNCTIONING ***
*/

#define SECURE_MODE

#define INCLUDE_FROM_BOOTLOADER_C
#include "Bootloader.h"

#if defined(SECURE_MODE)
bool          IsSecure      = true;
#endif

bool          RunBootloader = true;

uint8_t       DFU_State     = dfuIDLE;
uint8_t       DFU_Status    = OK;

DFU_Command_t SentCommand;
uint8_t       ResponseByte;

FuncPtr_t     AppStartPtr   = 0x0000;

uint8_t       Flash64KBPage = 0;
uint16_t      StartAddr     = 0x0000;
uint16_t      EndAddr       = 0x0000;

uint8_t       SigBytes[3];

int main (void)
{
	/* Disable watchdog if enabled by bootloader/fuses */
	MCUSR &= ~(1 << WDRF);
	wdt_disable();
	
	/* Relocate the interrupt vector table to the bootloader section */
	MCUCR = (1 << IVCE);
	MCUCR = (1 << IVSEL);

	/* Hardware initialization */
	Bicolour_Init();
	
	/* Staus LED set to red by default */
	Bicolour_SetLeds(BICOLOUR_LED1_RED);

	/* Initialize the USB subsystem */
	USB_Init();
	
	/* Read in signature bytes from the AVR */
	SigBytes[0] = boot_read_sig_byte(0);
	SigBytes[1] = boot_read_sig_byte(2);
	SigBytes[2] = boot_read_sig_byte(4);

	/* Run the USB management task while the bootload is supposed to be running */
	while (RunBootloader)
	  USB_USBTask();
	
	/* Shut down the USB subsystem */
	USB_ShutDown();
	
	/* Relocate the interrupt vector table back to the application section */
	MCUCR = (1 << IVCE);
	MCUCR = 0;

	/* Reset any used hardware ports back to their defaults */
	PORTD = 0;
	DDRD  = 0;
	
	/* Start the user application */
	boot_rww_enable();
	AppStartPtr();
}

EVENT_HANDLER(USB_Connect)
{
	/* Status LED green when USB connected */
	Bicolour_SetLed(1, BICOLOUR_LED1_GREEN);
	
	#if defined(SECURE_MODE)
	/* Set second LED to red colour to indicate secure mode is active */
	Bicolour_SetLed(2, BICOLOUR_LED2_RED);
	#endif
}

EVENT_HANDLER(USB_Disconnect)
{
	/* Upon disconnection, run user application */
	RunBootloader = false;
}

EVENT_HANDLER(USB_UnhandledControlPacket)
{
	uint8_t* CommandDataPtr = (uint8_t*)&SentCommand.Data;

	Endpoint_Ignore_Word();
	Endpoint_Ignore_Word();

	SentCommand.DataSize = Endpoint_Read_Word_LE();

	/* Processing request - status LED orange */
	Bicolour_SetLed(1, BICOLOUR_LED1_ORANGE);

	switch (Request)
	{
		case DFU_DNLOAD:
			Endpoint_ClearSetupReceived();
			
			if (DFU_State == dfuIDLE)
			{
				/* If the request has a data stage, load it into the command struct */
				if (SentCommand.DataSize)
				{
					while (!(Endpoint_Setup_Out_IsReceived()));

					/* First byte of the data stage is the DNLOAD request's command */
					SentCommand.Command = Endpoint_Read_Byte();
					
					/* One byte of the data stage is the command, so subtract it from the total data bytes */
					SentCommand.DataSize--;
					
					/* Load in the rest of the data stage as command parameters */
					for (uint16_t DataByte = 0; DataByte < SentCommand.DataSize; DataByte++)
					  *(CommandDataPtr++) = Endpoint_Read_Byte();
					
					Endpoint_Setup_Out_Clear();
				}

				/* Process the command */
				ProcessBootloaderCommand();
			}
			else
			{	
				DFU_State = dfuIDLE;
				// TODO
			}

			/* Send ZLP to the host to acknowedge the request */
			Endpoint_Setup_In_Clear();
			while (!(Endpoint_Setup_In_IsReady()));
				
			break;
		case DFU_UPLOAD:
			Endpoint_ClearSetupReceived();

			if (DFU_State == dfuIDLE)
			{
				Endpoint_Write_Byte(ResponseByte);
			}
			else
			{
				if (IS_ONEBYTE_COMMAND(SentCommand.Data, 0x00) ||          // Read FLASH
				    IS_ONEBYTE_COMMAND(SentCommand.Data, 0x02))            // Read EEPROM
				{
					while (SentCommand.DataSize-- && (StartAddr != EndAddr))
					{
						if (Endpoint_BytesInEndpoint() == ENDPOINT_CONTROLEP_SIZE)
						  Endpoint_Setup_In_Clear();
						  
						if (IS_ONEBYTE_COMMAND(SentCommand.Data, 0x02)) // Read EEPROM
						  Endpoint_Write_Byte(eeprom_read_byte((uint8_t*)StartAddr));
						else                                            // Read FLASH
						  Endpoint_Write_Byte(pgm_read_byte_far(((uint32_t)Flash64KBPage << 16) | StartAddr));						
						
						StartAddr++;
					}

					if (StartAddr == EndAddr)
					  DFU_State = dfuIDLE;
				}
				else if (IS_ONEBYTE_COMMAND(SentCommand.Data, 0x01)) // Blank Check
				{
					/* Blank checking is performed in the DFU_DNLOAD request - if we get here we've told the host
					   that the memory isn't blank, and the host is requesting the first non-blank address */
					Endpoint_Write_Word_LE(StartAddr);
					
					DFU_State = dfuIDLE;
				}
			}

			Endpoint_Setup_In_Clear();

			while (!(Endpoint_Setup_Out_IsReceived()));
			Endpoint_Setup_Out_Clear();

			break;
		case DFU_GETSTATUS:
			Endpoint_ClearSetupReceived();
			
			Endpoint_Write_Byte(DFU_Status);
			
			/* Write 24-bit poll timeout value */
			Endpoint_Write_Byte(0);
			Endpoint_Write_Word_LE(0);
			
			/* Write 8-bit state value */
			Endpoint_Write_Byte(DFU_State);

			/* Write 8-bit state string ID number */
			Endpoint_Write_Byte(0);

			Endpoint_Setup_In_Clear();
			
			while (!(Endpoint_Setup_Out_IsReceived()));
			Endpoint_Setup_Out_Clear();
	
			break;		
		case DFU_CLRSTATUS:
			Endpoint_ClearSetupReceived();
			
			/* Reset the status value variable to the default OK status */
			DFU_Status = OK;
			
			Endpoint_Setup_In_Clear();
			while (!(Endpoint_Setup_In_IsReady()));

			break;
		case DFU_GETSTATE:
			Endpoint_ClearSetupReceived();
			
			/* Write the current device state to the endpoint */
			Endpoint_Write_Byte(DFU_State);
		
			Endpoint_Setup_In_Clear();
			
			while (!(Endpoint_Setup_Out_IsReceived()));
			Endpoint_Setup_Out_Clear();

			break;
		case DFU_ABORT:
			Endpoint_ClearSetupReceived();
			
			/* Reset the current state variable to the default idle state */
			DFU_State = dfuIDLE;
			
			Endpoint_Setup_In_Clear();
			while (!(Endpoint_Setup_In_IsReady()));

			break;
	}

	/* End of request processing - status LED green */
	Bicolour_SetLed(1, BICOLOUR_LED1_GREEN);

	#if defined(SECURE_MODE)
	/* If secure mode is active, second LED is red, otherwise off */
	if (IsSecure)
	  Bicolour_SetLed(2, BICOLOUR_LED2_RED);
	else
	  Bicolour_SetLed(2, BICOLOUR_LED2_OFF);
	#endif
}

static void ProcessBootloaderCommand(void)
{
	#if defined(SECURE_MODE)
	/* Check if device is in secure mode */
	if (IsSecure)
	{
		/* Don't process command unless it is a READ or chip erase command */
		if (!(((SentCommand.Command == COMMAND_WRITE)             &&
		        IS_TWOBYTE_COMMAND(SentCommand.Data, 0x00, 0xFF)) ||
			   (SentCommand.Command == COMMAND_READ)))
		{
			/* Set the state and status variables to indicate the error */
			DFU_State  = dfuERROR;
			DFU_Status = errCHECK_ERASED;
			
			/* Stall command */
			Endpoint_StallTransaction();
			
			/* Don't process the command */
			return;
		}
	}
	#endif

	/* Dispatch the required command processing routine based on the command type */
	switch (SentCommand.Command)
	{
		case COMMAND_WRITE:
			ProcessWriteCommand();
			break;
		case COMMAND_READ:
			ProcessReadCommand();
			break;
		case COMMAND_PROG_START:
			ProcessMemProgCommand();
			break;
		case COMMAND_DISP_DATA:
			ProcessMemReadCommand();
			break;
		case COMMAND_CHANGE_BASE_ADDR:
			if (IS_TWOBYTE_COMMAND(SentCommand.Data, 0x03, 0x00)) // Set 64KB flash page command
			  Flash64KBPage = SentCommand.Data[2];

			break;
	}
}

static void ProcessMemProgCommand(void)
{
	/* Load in the start and ending programming addresses */
	StartAddr = (((uint16_t)SentCommand.Data[1] << 8) | SentCommand.Data[2]);
	EndAddr   = (((uint16_t)SentCommand.Data[3] << 8) | SentCommand.Data[4]);

	if (IS_ONEBYTE_COMMAND(SentCommand.Data, 0x00) ||             // Write FLASH command
		IS_ONEBYTE_COMMAND(SentCommand.Data, 0x01))
	{
		DFU_State = dfuDNLOAD_IDLE;
	}
}

static void ProcessMemReadCommand(void)
{
	/* Load in the start and ending reading addresses */
	StartAddr = (((uint16_t)SentCommand.Data[1] << 8) | SentCommand.Data[2]);
	EndAddr   = (((uint16_t)SentCommand.Data[3] << 8) | SentCommand.Data[4]);
	
	if (IS_ONEBYTE_COMMAND(SentCommand.Data, 0x00) ||             // Read FLASH command
        IS_ONEBYTE_COMMAND(SentCommand.Data, 0x02))               // Read EEPROM command
	{
		DFU_State = dfuUPLOAD_IDLE;
	}
	else if (IS_ONEBYTE_COMMAND(SentCommand.Data, 0x01))          // Blank check FLASH command
	{
		do
		{
			/* Construct 24-bit far address from current flash page and current address */
			uint32_t CurrAddress = (((uint32_t)Flash64KBPage << 16) | StartAddr);
			
			/* Check if the current byte is not blank */
			if (pgm_read_byte_far(CurrAddress) != 0xFF)
			{
				/* Set state and status variables to the appropriate error values */
				DFU_State  = dfuERROR;
				DFU_Status = errCHECK_ERASED;

				break;
			}
			
			StartAddr++;
		}
		while (StartAddr != EndAddr);
	}
}

static void ProcessWriteCommand(void)
{
	if (IS_TWOBYTE_COMMAND(SentCommand.Data, 0x03, 0x00))         // Start application, hardware reset
	{
		/* Check if empty request data array - an empty request after a filled request retains the
		   previous valid request data, but initializes the reset */
		if (!(SentCommand.DataSize))
		{
			/* Start the watchdog, enter infinite loop to reset the AVR */
			wdt_enable(WDTO_250MS);
			for (;;);
		}
	}
	else if (IS_TWOBYTE_COMMAND(SentCommand.Data, 0x03, 0x01))    // Start application, software jump
	{
		/* Check if empty request data array - an empty request after a filled request retains the
		   previous valid request data, but initializes the jump */
		if (!(SentCommand.DataSize))
		{
			/* Load in the jump address into the application start address pointer */
			AppStartPtr = (FuncPtr_t)((SentCommand.Data[2] << 8) | SentCommand.Data[3]);
			
			/* Set the flag to terminate the bootloader at next opportunity */
			RunBootloader = false;
		}	
	}
	else if (IS_TWOBYTE_COMMAND(SentCommand.Data, 0x00, 0xFF))    // Erase flash
	{
		EraseFlash();
					
		#if defined(SECURE_MODE)
		IsSecure = false;
		#endif
	}
}

static void ProcessReadCommand(void)
{
	ResponseByte = 0x00;

	if (IS_TWOBYTE_COMMAND(SentCommand.Data, 0x00, 0x00))         // Read bootloader info
	{
		ResponseByte = BOOTLOADER_VERSION;
	}
	else if (IS_ONEBYTE_COMMAND(SentCommand.Data, 0x01))          // Read signature byte
	{
		if (SentCommand.Data[1] == 0x30)                          // Read byte 1
		  ResponseByte = SigBytes[0];
		else if (SentCommand.Data[1] == 0x31)                     // Read byte 2
		  ResponseByte = SigBytes[1];
		else if (SentCommand.Data[1] == 0x60)                     // Read byte 3
		  ResponseByte = SigBytes[2];
	}
}

static void EraseFlash(void)
{
	uint32_t FlashAddr = 0;

	/* Clear the application section of flash */
	while (FlashAddr < BOOT_START_ADDR)
	{
		boot_page_erase(FlashAddr);
		boot_spm_busy_wait();
		boot_page_write(FlashAddr);
		boot_spm_busy_wait();

		FlashAddr += SPM_PAGESIZE;
	}
}