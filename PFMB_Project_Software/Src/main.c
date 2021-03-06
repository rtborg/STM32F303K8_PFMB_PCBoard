/*
 * PFMB7201 0-5V to RS485 Converter board
 * */

/*
 * JTAG CONNECTION. See https://www.segger.com/downloads/application-notes/AN00021
 * BOARD JTAG HEADER				JLINK PROBE
 * 1 - Vdd							1 - VTref
 * 2 - Jtms/SWDIO					7 - SWDIO
 * 3 - GND							4 - GND
 * 4 - Jtck/SWCLK					9 - SWCLK
 * 5 - NC
 * 6 - Jtdo/SWO						13 - SWO
 * 7 - NRST							15 - RST
 * 8 - Jtdi							5 - TDI
 * 9 - NC
 * 10 - Njtrst						3 - nTRST
 * */

#include "main.h"
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

/* Private typedef -----------------------------------------------------------*/
//Modbus command structure definition and buffer
typedef struct ModbusCommand {
	uint8_t		address;
	uint8_t		function_code;
	uint8_t		data[4];
	uint8_t		crc[2];
}ModbusCommand;
/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
#define UART_RX_BUFFER_LEN	8
#define UART_TX_BUFFER_LEN	64
#define MODBUS_COMMAND_BUFFER_LEN	8


/* Private variables ---------------------------------------------------------*/
ADC_HandleTypeDef hadc2;
CRC_HandleTypeDef hcrc;
OPAMP_HandleTypeDef hopamp2;
UART_HandleTypeDef huart1;
uint32_t modbus_address = 0;
float adc_value;

/* UART receive buffer */
// NOTE: Buffers are volatile, modified in interrupts
static volatile uint8_t uart_rx_buffer[UART_RX_BUFFER_LEN];
static volatile uint8_t uart_rx_buffer_head = 0;
static volatile uint8_t uart_rx_buffer_tail = 0;
static volatile uint8_t uart_rx_buffer_count = 0;
static volatile uint8_t uart_rx_buffer_head_old = 0;
bool rx_complete = false;

/* UART transmit buffer */
static volatile uint8_t uart_tx_buffer[UART_RX_BUFFER_LEN];
static volatile uint8_t uart_tx_buffer_head = 0;
static volatile uint8_t uart_tx_buffer_tail = 0;
static volatile uint8_t uart_tx_buffer_remaining = UART_RX_BUFFER_LEN;

/* Modbus commands buffer */
static volatile ModbusCommand modbus_command_buffer[MODBUS_COMMAND_BUFFER_LEN];
static volatile uint8_t modbus_command_buffer_head = 0;
static volatile uint8_t modbus_command_buffer_tail = 0;
static volatile uint8_t modbus_command_buffer_count = 0;

/* PFMB7201 variables */
static float sensor_step_per_liter = 0;
static float sensor_zero_offset = 0;

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_ADC2_Init(void);
static void MX_OPAMP2_Init(void);
static void MX_USART1_UART_Init(void);
static void MX_CRC_Init(void);

/* Modbus functions */
uint8_t get_modbus_address();
ModbusCommand get_modbus_command();

/* ADC functions */
float get_adc_value();

/* PFMB functions */
bool sensor_self_calibration(float *spl, float *zo);

/* UART functions */
uint8_t uart_getchar();
bool uart_has_data();
void uart_putchar(uint8_t ch);
void uart_putstring(uint8_t *s, uint8_t size);


int main(void)
{

  /* MCU Configuration--------------------------------------------------------*/
  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();
  /* Configure the system clock */
  SystemClock_Config();
  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_ADC2_Init();
  MX_OPAMP2_Init();
  MX_USART1_UART_Init();
  MX_CRC_Init();
  HAL_OPAMP_Start(&hopamp2);

  modbus_address = get_modbus_address();

  if (sensor_self_calibration(&sensor_step_per_liter, &sensor_zero_offset) == false) {
	  Error_Handler();
  }


  while (1)
  {
	  // Check if a message is received
	  if ( modbus_command_buffer_count != 0 ) {
		  // Get modbus command from buffer
		  ModbusCommand mc = get_modbus_command();
		  // Check for valid function code
		  if (mc.function_code != 0x00) {
			  HAL_GPIO_TogglePin(Keepalive_LED_GPIO_Port, Keepalive_LED_Pin);
			  // If this is a flow request, respond
			  if ( mc.function_code == 0x04 &&
					  mc.data[0] == 0x0 &&
					  mc.data[1] == 0x01 &&
					  mc.data[2] == 0x0 &&
					  mc.data[3] == 0x01) {
				  uint8_t response[9] = {};												// Create response array
				  response[0] = mc.address;												// Copy device address
				  response[1] = mc.function_code;										// Copy function code
				  response[2] = 2;
				  float flow = get_adc_value();	// ! NEEDS TO BE A FLOAT
				  flow = (flow - sensor_zero_offset) / sensor_step_per_liter;			// Calculate flow in liters
				  flow = round(flow);
				  uint16_t flow_t = (int16_t) flow;
				  response[3] = (uint8_t) (flow_t >> 8);							// Copy measurement in buffer
				  response[4] = (uint8_t) (flow_t & 0xff);
				  uint16_t crc = HAL_CRC_Calculate(&hcrc, (uint32_t *) response, 5);
				  response[5] = (uint8_t) (crc & 0xff);									// Copy CRC in buffer
				  response[6] = (uint8_t) (crc >> 8);
				  HAL_UART_Transmit(&huart1, response, 7, 1000);
			  }
		  }
		  __NOP();
	  }

  }
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};
  RCC_PeriphCLKInitTypeDef PeriphClkInit = {0};

  /** Initializes the CPU, AHB and APB busses clocks 
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL9;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }
  /** Initializes the CPU, AHB and APB busses clocks 
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
  PeriphClkInit.PeriphClockSelection = RCC_PERIPHCLK_USART1|RCC_PERIPHCLK_ADC12;
  PeriphClkInit.Usart1ClockSelection = RCC_USART1CLKSOURCE_PCLK1;
  PeriphClkInit.Adc12ClockSelection = RCC_ADC12PLLCLK_DIV1;
  if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInit) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief ADC2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_ADC2_Init(void)
{
  ADC_ChannelConfTypeDef sConfig = {0};
  /** Common config 
  */
  hadc2.Instance = ADC2;
  hadc2.Init.ClockPrescaler = ADC_CLOCK_ASYNC_DIV1;
  hadc2.Init.Resolution = ADC_RESOLUTION_12B;
  hadc2.Init.ScanConvMode = ADC_SCAN_DISABLE;
  hadc2.Init.ContinuousConvMode = DISABLE;
  hadc2.Init.DiscontinuousConvMode = DISABLE;
  hadc2.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_NONE;
  hadc2.Init.ExternalTrigConv = ADC_SOFTWARE_START;
  hadc2.Init.DataAlign = ADC_DATAALIGN_RIGHT;
  hadc2.Init.NbrOfConversion = 1;
  hadc2.Init.DMAContinuousRequests = DISABLE;
  hadc2.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
  hadc2.Init.LowPowerAutoWait = DISABLE;
  hadc2.Init.Overrun = ADC_OVR_DATA_OVERWRITTEN;
  if (HAL_ADC_Init(&hadc2) != HAL_OK)
  {
    Error_Handler();
  }
  /** Configure Regular Channel 
  */
  sConfig.Channel = ADC_CHANNEL_3;
  sConfig.Rank = ADC_REGULAR_RANK_1;
  sConfig.SingleDiff = ADC_SINGLE_ENDED;
  sConfig.SamplingTime = ADC_SAMPLETIME_181CYCLES_5;
  sConfig.OffsetNumber = ADC_OFFSET_NONE;
  sConfig.Offset = 0;
  if (HAL_ADC_ConfigChannel(&hadc2, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN ADC2_Init 2 */
  // Calibration as per https://www.youtube.com/watch?v=qqGsy06mris
  ADC2->CR &= ~ADC_CR_ADEN;			// Disable ADC
  ADC2->CR |= ADC_CR_ADCAL;			// Start calibration
  while ( (ADC2->CR & ADC_CR_ADCAL) != 0);
  ADC2->CR |= ADC_CR_ADEN;			// Enable ADC
  /* USER CODE END ADC2_Init 2 */

}

/**
  * @brief CRC Initialization Function
  * @param None
  * @retval None
  */
static void MX_CRC_Init(void)
{
  hcrc.Instance = CRC;
  hcrc.Init.DefaultPolynomialUse = DEFAULT_POLYNOMIAL_DISABLE;
  hcrc.Init.DefaultInitValueUse = DEFAULT_INIT_VALUE_DISABLE;
  hcrc.Init.GeneratingPolynomial = 32773;
  hcrc.Init.CRCLength = CRC_POLYLENGTH_16B;
  hcrc.Init.InitValue = 0xffff;
  hcrc.Init.InputDataInversionMode = CRC_INPUTDATA_INVERSION_BYTE;
  hcrc.Init.OutputDataInversionMode = CRC_OUTPUTDATA_INVERSION_ENABLE;
  hcrc.InputDataFormat = CRC_INPUTDATA_FORMAT_BYTES;
  if (HAL_CRC_Init(&hcrc) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief OPAMP2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_OPAMP2_Init(void)
{
  hopamp2.Instance = OPAMP2;
  hopamp2.Init.Mode = OPAMP_FOLLOWER_MODE;
  hopamp2.Init.NonInvertingInput = OPAMP_NONINVERTINGINPUT_IO2;
  hopamp2.Init.TimerControlledMuxmode = OPAMP_TIMERCONTROLLEDMUXMODE_DISABLE;
  hopamp2.Init.UserTrimming = OPAMP_TRIMMING_FACTORY;
  if (HAL_OPAMP_Init(&hopamp2) != HAL_OK)
  {
    Error_Handler();
  }

}

/**
  * @brief USART1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART1_UART_Init(void)
{
  huart1.Instance = USART1;
  huart1.Init.BaudRate = 9600;
  huart1.Init.WordLength = UART_WORDLENGTH_8B;
  huart1.Init.StopBits = UART_STOPBITS_2;
  huart1.Init.Parity = UART_PARITY_NONE;
  huart1.Init.Mode = UART_MODE_TX_RX;
  huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart1.Init.OverSampling = UART_OVERSAMPLING_16;
  huart1.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
  huart1.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
  if (HAL_RS485Ex_Init(&huart1, UART_DE_POLARITY_HIGH, 16, 16) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART1_Init 2 */
  HAL_UART_ReceiverTimeout_Config(&huart1, 960);				// Set receiver timeout value
  HAL_UART_EnableReceiverTimeout(&huart1);						// Enable receiver timeout interrupt for Modbus
  __HAL_UART_CLEAR_FLAG(&huart1, UART_FLAG_RTOF);				// Clear RTOF flag
  __HAL_UART_ENABLE_IT(&huart1, UART_IT_RTO);					// Enable Receive Timeout interrupt

  __HAL_UART_ENABLE_IT(&huart1, UART_IT_RXNE);					// Enable Receive interrupt
  HAL_NVIC_SetPriority(USART1_IRQn, 0, 0);						// Set interrupt priority
   HAL_NVIC_EnableIRQ(USART1_IRQn);								// Enable interrupts
  /* USER CODE END USART1_Init 2 */

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOF_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(Keepalive_LED_GPIO_Port, Keepalive_LED_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pins : RS485_ADDR_7_Pin RS485_ADDR_6_Pin RS485_ADDR_5_Pin RS485_ADDR_4_Pin 
                           RS485_ADDR_3_Pin RS485_ADDR_2_Pin */
  GPIO_InitStruct.Pin = RS485_ADDR_7_Pin|RS485_ADDR_6_Pin|RS485_ADDR_5_Pin|RS485_ADDR_4_Pin 
                          |RS485_ADDR_3_Pin|RS485_ADDR_2_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /*Configure GPIO pin : RS485_ADDR_1_Pin */
  GPIO_InitStruct.Pin = RS485_ADDR_1_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(RS485_ADDR_1_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pin : RS485_ADDR_0_Pin */
  GPIO_InitStruct.Pin = RS485_ADDR_0_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(RS485_ADDR_0_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pin : Keepalive_LED_Pin */
  GPIO_InitStruct.Pin = Keepalive_LED_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(Keepalive_LED_GPIO_Port, &GPIO_InitStruct);

}

/* USER CODE BEGIN 4 */
uint8_t get_modbus_address() {
	// Read address pin states
	uint8_t m_addr = 0;

	m_addr |= (uint8_t) ( HAL_GPIO_ReadPin(RS485_ADDR_0_GPIO_Port, RS485_ADDR_0_Pin) << 0);
	m_addr |= (uint8_t) ( HAL_GPIO_ReadPin(RS485_ADDR_1_GPIO_Port, RS485_ADDR_1_Pin) << 1);
	m_addr |= (uint8_t) ( HAL_GPIO_ReadPin(RS485_ADDR_2_GPIO_Port, RS485_ADDR_2_Pin) << 2);
	m_addr |= (uint8_t) ( HAL_GPIO_ReadPin(RS485_ADDR_3_GPIO_Port, RS485_ADDR_3_Pin) << 3);
	m_addr |= (uint8_t) ( HAL_GPIO_ReadPin(RS485_ADDR_4_GPIO_Port, RS485_ADDR_4_Pin) << 4);
	m_addr |= (uint8_t) ( HAL_GPIO_ReadPin(RS485_ADDR_5_GPIO_Port, RS485_ADDR_5_Pin) << 5);
	m_addr |= (uint8_t) ( HAL_GPIO_ReadPin(RS485_ADDR_6_GPIO_Port, RS485_ADDR_6_Pin) << 6);
	m_addr |= (uint8_t) ( HAL_GPIO_ReadPin(RS485_ADDR_7_GPIO_Port, RS485_ADDR_7_Pin) << 7);

	// Invert lower 8 bits as the switch is connected backwards...
	m_addr = ~m_addr;

	return m_addr;
}

float get_adc_value() {
	float adc_val = 0;

	HAL_ADC_Start(&hadc2);
	if (HAL_ADC_PollForConversion(&hadc2, 100) != HAL_OK) {
		return -1;
	}

	adc_val = (float) (HAL_ADC_GetValue(&hadc2) / 4095.0) * 3.3000;
	return adc_val;
}

/**
 * @brief USART1 interrupt service routine
 */
void USART1_IRQHandler(void) {

	/* Check if this is a receive buffer not empty interrupt */
	if ( __HAL_UART_GET_FLAG(&huart1, UART_FLAG_RXNE) ) {

		uart_rx_buffer[uart_rx_buffer_head++] = USART1->RDR;			// Place byte into receive buffer
		if (uart_rx_buffer_head == UART_RX_BUFFER_LEN) {
			uart_rx_buffer_head = 0;
		}

		__HAL_UART_CLEAR_FLAG(&huart1, UART_FLAG_RTOF);

		uart_rx_buffer_count++;
	} else if (__HAL_UART_GET_FLAG(&huart1, UART_FLAG_RTOF)) {					// The Receive Timeout interrupt happens when an idle time of more than 40 bits (3.5 modbus 11 bit chars)
		__HAL_UART_CLEAR_FLAG(&huart1, UART_FLAG_RTOF);

		if ( ( uart_rx_buffer_head == 0 ) && ( uart_rx_buffer_count == 8 ) ) {						// Check modbus command buffer. If head == 0 and count == 8, an 8-byte command has been received and head has wrapped around
			uart_rx_buffer_count = 0;																// Zero uart rx buffer count
			if ( uart_rx_buffer[0] == modbus_address ) {											// If the command is addressed to this device, put it into buffer
				memcpy((void *)&modbus_command_buffer[modbus_command_buffer_head], (void *)&uart_rx_buffer, 8);		// Copy UART Rx buffer into modbus commands buffer and wrap counters
				modbus_command_buffer_head++;
				modbus_command_buffer_count++;
				if ( modbus_command_buffer_head == MODBUS_COMMAND_BUFFER_LEN ) modbus_command_buffer_head = 0;
				if ( modbus_command_buffer_count == MODBUS_COMMAND_BUFFER_LEN ) modbus_command_buffer_count = 0;
			}
		} else {
			uart_rx_buffer_head = uart_rx_buffer_count = 0;							// If length and count are not as expected, clear them. Interrupt must be a result of spurious activity
		}
	}

	/* Handle transmit interrupt */
//	if ( __HAL_UART_GET_FLAG(&huart1, UART_FLAG_TXE) ) {
//		if ( uart_tx_buffer_remaining < UART_TX_BUFFER_LEN ) {			// If the number of free spaces in the buffer is less than the size of the buffer that means there's still characters to be sent
//			huart1.Instance->TDR = uart_tx_buffer[uart_tx_buffer_tail++];	// Place char in the TX buffer. This also clears the interrupt flag
//			if (uart_tx_buffer_tail >= UART_TX_BUFFER_LEN) {
//				uart_tx_buffer_tail = 0;								// Wrap around tail if needed
//			}
//			uart_tx_buffer_remaining++;									// Increase number of remaining characters
//		} else {														// If remaining chars == buffer size, there's nothing to transmit
//			huart1.Instance->CR1 &= ~USART_CR1_TXEIE;					// Disable TXE interrupt
//			__HAL_UART_CLEAR_FLAG(&huart1, UART_FLAG_TXE);
//		}
//	}

	if (__HAL_UART_GET_FLAG(&huart1, UART_FLAG_ORE)) {					// Clear overrun flag
		__HAL_UART_CLEAR_FLAG(&huart1, UART_FLAG_ORE);
	}

	if (__HAL_UART_GET_FLAG(&huart1, UART_FLAG_FE)) {					// Clear frame error flag
		__HAL_UART_CLEAR_FLAG(&huart1, UART_FLAG_FE);
	}
}

uint8_t uart_getchar() {
	uint8_t read_val = 0;

	while (0 == uart_rx_buffer_count) {

	}

	read_val = uart_rx_buffer[uart_rx_buffer_tail++];

	if (uart_rx_buffer_tail >= UART_RX_BUFFER_LEN) {
		uart_rx_buffer_tail = 0;
	}

	__HAL_UART_DISABLE_IT(&huart1, UART_IT_RXNE);
	uart_rx_buffer_count--;
	__HAL_UART_ENABLE_IT(&huart1, UART_IT_RXNE);

	return read_val;
}

bool uart_has_data() {
	return (uart_rx_buffer_count ? true : false);
}

void uart_putchar(uint8_t ch) {
	while (0 == uart_tx_buffer_remaining) continue;			// Wait until there's a free space in the transmit buffer

	if (0 == ( huart1.Instance->CR1 & USART_CR1_TXEIE) ) {	// If TXE interrupt is disabled, directly put char in USART TDR
		huart1.Instance->TDR = ch;
	} else {
		huart1.Instance->CR1 &= ~USART_CR1_TXEIE;			// Disable TXE interrupt temporarily TBC
		uart_tx_buffer[uart_tx_buffer_head++] = ch;			// Place data in buffer
		if (uart_tx_buffer_head >= UART_TX_BUFFER_LEN) {	// Wrap around buffer head
			uart_tx_buffer_head = 0;
		}
		uart_tx_buffer_remaining--;							// Decrease number of free spaces in buffer
	}

	huart1.Instance->CR1 |= USART_CR1_TXEIE;				// Enable TXE interrupt
}

void uart_putstring(uint8_t *s, uint8_t size) {
	for (int i = 0; i < size; i++) {
		uart_putchar(s[i]);
	}
}

/**
 * @brief Get next available modbus command from the buffer. CRC is checked internally
 * @return ModbusCommand as copied from the buffer. If the function code is set to 0x00, there was no command in the buffer, or the CRC was wrong
 */
ModbusCommand get_modbus_command() {
	ModbusCommand m_command;									// Create an empty modbus command structure
	m_command.function_code = 0x00;								// Set function code to invalid value

	if (modbus_command_buffer_count > 0) {						// If there's something in the buffer, copy it in the modbus command struct
		memcpy ((void *)&m_command, (void *) &modbus_command_buffer[modbus_command_buffer_tail], 8);
		modbus_command_buffer_tail++;							// Modify buffer counters
		modbus_command_buffer_count--;
		if (modbus_command_buffer_tail == MODBUS_COMMAND_BUFFER_LEN) modbus_command_buffer_tail = 0;

		uint32_t message_crc =  ((uint32_t) m_command.crc[1] << 8UL) | (m_command.crc[0]);		// Copy message CRC in a 32-bit variable
		uint32_t calculated_crc = HAL_CRC_Calculate(&hcrc, (uint32_t *) &m_command, 6);
		if ( calculated_crc != message_crc ) m_command.function_code = 0x00;					// If calculated CRC is different, change function code to signal invalid CRC
	}

	return m_command;
}

bool sensor_self_calibration(float *spl, float *zo) {
	*zo = get_adc_value();								// Get ADC value when sensor is not reading the flow

	if ( (*zo == -1) || (*zo < 0.6) ) return false;		// If the value is anything less than 0.6, calibration has failed

	*spl = (float) (3.300 - *zo) / 200.0;				// Calculate ADC steps per liter

	return true;
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart) {
	rx_complete = true;
	HAL_UART_Receive_IT(&huart1, uart_rx_buffer, UART_RX_BUFFER_LEN);
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart) {
	if (huart->ErrorCode & HAL_UART_ERROR_RTO) {
		rx_complete = true;
		HAL_UART_Receive_IT(&huart1, uart_rx_buffer, UART_RX_BUFFER_LEN);
	} else {
		HAL_UART_Receive_IT(&huart1, uart_rx_buffer, UART_RX_BUFFER_LEN);
	}
}

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
	while(1);
}

