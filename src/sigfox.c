/* ==========================================================
 * sigfox.c - Abstraction layer for sigfox libraries
 * Project : Disk91 SDK
 * ----------------------------------------------------------
 * Created on: 04 nov. 2018
 *     Author: Paul Pinault aka Disk91
 * ----------------------------------------------------------
 * Copyright (C) 2018 Disk91
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU LESSER General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Lesser Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 * ----------------------------------------------------------
 * 
 *
 * ==========================================================
 */
#include <stdbool.h>
#include <string.h>

#include <it_sdk/config.h>
#if ( ITSDK_WITH_SIGFOX_LIB == __ENABLE ) && (ITSDK_SIGFOX_LIB == __SIGFOX_SX1276)

// Hardware layer
SPI_HandleTypeDef hspi1;
DMA_HandleTypeDef hdma_spi1_tx;

#include <it_sdk/itsdk.h>
#include <it_sdk/sigfox/sigfox.h>
#include <it_sdk/logger/logger.h>
#include <it_sdk/logger/error.h>
#include <it_sdk/encrypt/encrypt.h>

#include <drivers/sigfox/sigfox_api.h>
#include <drivers/sigfox/se_nvm.h>
#include <it_sdk/eeprom/sdk_state.h>
#include <it_sdk/eeprom/sdk_config.h>

#include <drivers/sx1276/sigfox_sx1276.h>
#include <drivers/sigfox/mcu_api.h>
#include <sigfox_api.h>

static sigfox_api_t * __api;


// Some missing functions
itsdk_error_ret_e itsdk_error_noreport(uint32_t error) {
	if ( (error & ITSDK_ERROR_LEVEL_FATAL ) == ITSDK_ERROR_LEVEL_FATAL ) while(1);
	return ITSDK_ERROR_SUCCESS;
}

/**
 * Get the IRQ Mask
 */
uint32_t itsdk_getIrqMask() {
	return __get_PRIMASK();
}

/**
 * Set / Restore the IRQ Mask
 */
void itsdk_setIrqMask(uint32_t mask) {
	__set_PRIMASK(mask);
}
/**
 * Enter a critical section / disable interrupt
 */
static uint32_t __interrupt_mask;
void itsdk_enterCriticalSection() {
	__interrupt_mask = itsdk_getIrqMask();
	//__disable_irq();
	__set_PRIMASK(1);	// allows to capture but not execute the interruption appearing during the critical section execution
}

/**
 * Restore the initial irq mask
 * to leave a critical secqtion
 */
void itsdk_leaveCriticalSection() {
	itsdk_setIrqMask(__interrupt_mask);
}


// State
itsdk_state_t itsdk_state = {0};


// helper to get the Rc
static uint8_t _itsdk_sigfox_getRc()  {
	uint32_t region;
	__api->getCurrentRegion(&region);
	uint8_t rcz;
	if ( itsdk_sigfox_getRczFromRegion(region,&rcz) != SIGFOX_INIT_SUCESS ) {
		return 0;
	}
	return rcz;
} 


/**
 * Return the Tx power value updated when defaul
 */
static uint8_t _itsdk_sigfox_getTxPower() {
	int8_t power;
	__api->getTxPower(&power);
	if ( power == SIGFOX_DEFAULT_POWER ) {
		switch (_itsdk_sigfox_getRc()) {
		case SIGFOX_RCZ1:
		case SIGFOX_RCZ5:
			power = 14;
			break;
		case SIGFOX_RCZ2:
		case SIGFOX_RCZ4:
			power = 24;
			break;
		case SIGFOX_RCZ3C:
			power = 16;
			break;
		default:
			return 0;
		}
	}
	return power;
}

/**
 * Return the default speed according to the RC
 */ 
static uint16_t _itsdk_sigfox_getSpeed() {
	switch (itsdk_sigfox_getRc()) {
		case SIGFOX_RCZ1:
		case SIGFOX_RCZ3C:
		case SIGFOX_RCZ5:
			itsdk_state.sigfox.current_speed = SIGFOX_SPEED_100;
			break;
		case SIGFOX_RCZ2:
		case SIGFOX_RCZ4:
			itsdk_state.sigfox.current_speed = SIGFOX_SPEED_600;
			break;
		default:
			ITSDK_ERROR_REPORT(ITSDK_ERROR_SIGFOX_RCZ_NOTSUPPORTED,(uint16_t)itsdk_state.sigfox.rcz);
	}
}



/**
 * All operation needed to initialize the sigfox stack
 */
itsdk_sigfox_init_t sigfox_setup(sigfox_api_t * api) {
	LOG_INFO_SIGFOXSTK(("itsdk_sigfox_setup\r\n"));

	// Init the state
	itsdk_state.activeNetwork = __ACTIV_NETWORK_SIGFOX;
	if ( itsdk_state.sigfox.initialized ) return SIGFOX_INIT_NOCHANGE;
	if (    api == NULL
	     || api->getCurrentRegion == NULL
	     || api->getDeviceId == NULL
	     || api->getInitialPac == NULL
	     || api->getDeviceKey == NULL
	     || api->getCurrentSeqId == NULL
	     || api->setCurrentSeqId == NULL
	     || api->getTxPower == NULL
	) {
		return SIGFOX_INIT_FAILED;
	}
	__api = api;
	uint8_t rcz = _itsdk_sigfox_getRc();
	if (rcz == 0) return SIGFOX_INIT_FAILED;


	itsdk_sigfox_init_t ret = SIGFOX_INIT_SUCESS;
	ret = sx1276_sigfox_init();

	// Set the default power
	itsdk_state.sigfox.current_power = _itsdk_sigfox_getTxPower();
	itsdk_sigfox_setTxPower_ext(itsdk_state.sigfox.current_power,true);

	if ( ret == SIGFOX_INIT_SUCESS ) {
		itsdk_state.sigfox.initialized = true;
	}

	return ret;
}

void sigfox_loop() {
	#if ITSDK_TIMER_SLOTS > 0
	   itsdk_stimer_run();
	#endif
}

/**
 * Stop the sigfox stack and be ready for activating another stack
 */
itsdk_sigfox_init_t itsdk_sigfox_deinit() {
	sx1276_sigfox_deinit();
	itsdk_state.sigfox.initialized = true;
	return SIGFOX_INIT_SUCESS;
}


/**
 * Send a frame on sigfox network
 * buf - the buffer containing the data to be transmitted
 * len - the frame len in byte
 * repeat - the number of repeat expected (from 0 to 2)
 * speed - the transmission speed itdsk_sigfox_speed_t
 * power - transmission power -1 for default value
 * encrypt - type en encryption to apply to the data frame
 * ack - when true a downlink transmission is requested
 * dwn - downlink buffer for downlink reception
 */
itdsk_sigfox_txrx_t itsdk_sigfox_sendFrame(
		uint8_t * buf,
		uint8_t len,
		uint8_t repeat,
		itdsk_sigfox_speed_t speed,
		int8_t power,
		itdsk_payload_encrypt_t encrypt,
		bool ack,
		uint8_t * dwn
) {
	LOG_INFO_SIGFOXSTK(("itsdk_sigfox_sendFrame\r\n"));

	// some basic checking...
	if ( len > 12) return SIGFOX_ERROR_PARAMS;
	if ( repeat > 2) repeat = 2;
	if ( power == SIGFOX_POWER_DEFAULT ) power = itsdk_state.sigfox.current_power;
	if ( speed == SIGFOX_SPEED_DEFAULT ) speed = _itsdk_sigfox_getSpeed();
	if ( ack && (dwn == NULL)) return SIGFOX_ERROR_PARAMS;

	#if ( ITSDK_SIGFOX_ENCRYPTION & __PAYLOAD_ENCRYPT_SIGFOX)
	if ( (encrypt & PAYLOAD_ENCRYPT_SIGFOX) == 0 ) {
		log_error("[Sigfox] Sigfox ITSDK_SIGFOX_ENCRYPTION must be set as encryption has been activated\r\n");
		return SIGFOX_TXRX_ERROR;
	}
	#else
	if ( (encrypt & PAYLOAD_ENCRYPT_SIGFOX) != 0 ) {
		log_error("[Sigfox] Sigfox ITSDK_SIGFOX_ENCRYPTION can't be set until encryption has been activated\r\n");
		return SIGFOX_TXRX_ERROR;
	}
	#endif

	// Transmit the frame
	itsdk_sigfox_setTxPower(power);
	itsdk_sigfox_setTxSpeed(speed);

	itdsk_sigfox_txrx_t result;
	uint16_t ret = SIGFOX_API_send_frame(buf,len,dwn,repeat,ack);
	switch (ret&0xFF) {
	case SFX_ERR_INT_GET_RECEIVED_FRAMES_TIMEOUT:
		result = SIGFOX_TXRX_NO_DOWNLINK;
		break;
	case SFX_ERR_NONE:
		if ( ack ) {
			result = SIGFOX_TXRX_DOWLINK_RECEIVED;
		} else {
			result = SIGFOX_TRANSMIT_SUCESS;
		}
		break;
	default:
		result = SIGFOX_TXRX_ERROR;
		break;
	}
	return result;
}


/**
 * Send a frame on sigfox network
 * buf - the buffer containing the data to be transmitted
 * len - the frame len in byte
 * repeat - the number of repeat expected (from 0 to 2)
 * speed - the transmission speed itdsk_sigfox_speed_t
 * power - transmission power -1 for default value
 * encrypt - type en encryption to apply to the data frame
 * ack - when true a downlink transmission is requested
 * dwn - downlink buffer for downlink reception
 */
itdsk_sigfox_txrx_t itsdk_sigfox_sendBit(
		bool bitValue,
		uint8_t repeat,
		itdsk_sigfox_speed_t speed,
		int8_t power,
		bool ack,
		uint8_t * dwn
) {
	LOG_INFO_SIGFOXSTK(("itsdk_sigfox_sendBit\r\n"));

	// some basic checking...
	if ( repeat > 2) repeat = 2;
	if ( power == SIGFOX_POWER_DEFAULT ) power = itsdk_state.sigfox.current_power;
	if ( speed == SIGFOX_SPEED_DEFAULT ) speed = _itsdk_sigfox_getSpeed();
	if ( ack && dwn == NULL) return SIGFOX_ERROR_PARAMS;

	itsdk_sigfox_setTxPower(power);
	itsdk_sigfox_setTxSpeed(speed);

	itdsk_sigfox_txrx_t result = SIGFOX_TXRX_ERROR;
	uint16_t ret = SIGFOX_API_send_bit( bitValue,dwn,repeat,ack);
	switch (ret&0xFF) {
		case SFX_ERR_INT_GET_RECEIVED_FRAMES_TIMEOUT:
			result = SIGFOX_TXRX_NO_DOWNLINK;
			break;
		case SFX_ERR_NONE:
			if ( ack ) {
				result = SIGFOX_TXRX_DOWLINK_RECEIVED;
			} else {
				result = SIGFOX_TRANSMIT_SUCESS;
			}
			break;
		default:
			result = SIGFOX_TXRX_ERROR;
			break;
	}
	return result;
}

/**
 * Send an OOB message
 */
itdsk_sigfox_txrx_t itsdk_sigfox_sendOob(
		itdsk_sigfox_oob_t oobType,
		itdsk_sigfox_speed_t speed,
		int8_t power
) {
	LOG_INFO_SIGFOXSTK(("itsdk_sigfox_sendOob\r\n"));

	// some basic checking...
	if ( power == SIGFOX_POWER_DEFAULT ) power = itsdk_state.sigfox.current_power;
	if ( speed == SIGFOX_SPEED_DEFAULT ) speed = _itsdk_sigfox_getSpeed();
	itsdk_sigfox_setTxPower(power);
	itsdk_sigfox_setTxSpeed(speed);

	itdsk_sigfox_txrx_t result = SIGFOX_TXRX_ERROR;
	uint16_t ret=0;
	switch (oobType) {
		case SIGFOX_OOB_SERVICE:
			ret = SIGFOX_API_send_outofband(SFX_OOB_SERVICE);
			break;
		case SIGFOX_OOB_RC_SYNC:
			ret = SIGFOX_API_send_outofband(SFX_OOB_RC_SYNC);
			break;
		default:
			ITSDK_ERROR_REPORT(ITSDK_ERROR_SIGFOX_OOB_NOTSUPPORTED,(uint16_t)oobType);
		}
		switch (ret&0xFF) {
		case SFX_ERR_NONE:
			result = SIGFOX_TRANSMIT_SUCESS;
			break;
		default:
			result = SIGFOX_TXRX_ERROR;
			break;
	}
	return result;
}

/**
 * Get the current RCZ
 */
itsdk_sigfox_init_t itsdk_sigfox_getCurrentRcz(uint8_t * rcz) {
	LOG_INFO_SIGFOXSTK(("itsdk_sigfox_getCurrentRcz\r\n"));
	*rcz = _itsdk_sigfox_getRc();
	if ( rcz > 0 ) return SIGFOX_INIT_SUCESS;
	return SIGFOX_INIT_PARAMSERR;
}


/**
 * Change the transmission power to the given value
 */
itsdk_sigfox_init_t itsdk_sigfox_setTxPower_ext(uint8_t power, bool force) {
	LOG_INFO_SIGFOXSTK(("itsdk_sigfox_setTxPower_ext\r\n"));

	if ( !force && power == itsdk_state.sigfox.current_power ) return SIGFOX_INIT_NOCHANGE;
	sx1276_sigfox_setPower( power );
	itsdk_state.sigfox.current_power = power;
	return SIGFOX_INIT_SUCESS;
}

/**
 * Change the current sigfox network speed
 */
itsdk_sigfox_init_t itsdk_sigfox_setTxPower(uint8_t power) {
	LOG_DEBUG_SIGFOXSTK(("itsdk_sigfox_setTxPower\r\n"));
	return itsdk_sigfox_setTxPower_ext(power,false);
}

/**
 * Get the current sigfox trasnmision power
 */
itsdk_sigfox_init_t itsdk_sigfox_getTxPower(uint8_t * power) {
	LOG_DEBUG_SIGFOXSTK(("itsdk_sigfox_getTxPower\r\n"));
	*power = itsdk_state.sigfox.current_power;
	return SIGFOX_INIT_SUCESS;
}


/**
 * Change the transmission speed
 */
itsdk_sigfox_init_t itsdk_sigfox_setTxSpeed(itdsk_sigfox_speed_t speed) {
	LOG_INFO_SIGFOXSTK(("itsdk_sigfox_setTxSpeed\r\n"));
	// not yet supported
	LOG_WARN_SIGFOX(("Sigfox speed change not yet supported"));
	return SIGFOX_INIT_NOCHANGE;
}

/**
 * Get the current sigfox network speed
 */
itsdk_sigfox_init_t itsdk_sigfox_getTxSpeed(itdsk_sigfox_speed_t * speed) {
	LOG_DEBUG_SIGFOXSTK(("itsdk_sigfox_getTxSpeed\r\n"));
	*speed = (itdsk_sigfox_speed_t)_itsdk_sigfox_getSpeed();
	return SIGFOX_INIT_SUCESS;
}

/**
 * Return the deviceId into the given parameter
 */
itsdk_sigfox_init_t itsdk_sigfox_getDeviceId(itsdk_sigfox_device_is_t * devId) {
	LOG_INFO_SIGFOXSTK(("itsdk_sigfox_getDeviceId\r\n"));
	uint32_t _devId;
	__api->getDeviceId(&_devId);
	*devId = (itsdk_sigfox_device_is_t)_devId;
	return SIGFOX_INIT_SUCESS;
}

/**
 * Return the initial PAC into the given parameter
 * The PAC parameter is a 8 Bytes table
 */
itsdk_sigfox_init_t itsdk_sigfox_getInitialPac(uint8_t * pac) {
	LOG_INFO_SIGFOXSTK(("itsdk_sigfox_getInitialPac\r\n"));
	__api->getDeviceId(pac);
	return SIGFOX_INIT_SUCESS;
}

/**
 * Return the last reception RSSI into the given parameter
 * S2LP_UNKNOWN_RSSI if unknow (0x0F00);
 */
itsdk_sigfox_init_t itsdk_sigfox_getLastRssi(int16_t * rssi) {
	LOG_INFO_SIGFOXSTK(("itsdk_sigfox_getLastRssi\r\n"));

	sx1276_sigfox_getRssi(rssi);

	return SIGFOX_INIT_SUCESS;
}

/**
 * Return the last used seqId
 */
itsdk_sigfox_init_t itsdk_sigfox_getLastSeqId(uint16_t * seqId) {
	LOG_INFO_SIGFOXSTK(("itsdk_sigfox_getLastSeqId\r\n"));
	sx1276_sigfox_getSeqId(seqId);
	return SIGFOX_INIT_SUCESS;
}

/**
 * Return the next used seqId
 */
itsdk_sigfox_init_t itsdk_sigfox_getNextSeqId(uint16_t * seqId) {
	LOG_INFO_SIGFOXSTK(("itsdk_sigfox_getNextSeqId\r\n"));
	sx1276_sigfox_getSeqId(seqId);
	*seqId = (*seqId+1) & 0x0FFF;
	return SIGFOX_INIT_SUCESS;
}


/**
 * Switch to public key
 */
itsdk_sigfox_init_t itsdk_sigfox_switchPublicKey() {
	LOG_INFO_SIGFOXSTK(("itsdk_sigfox_switchPublicKey\r\n"));
	SIGFOX_API_switch_public_key(true);
	return SIGFOX_INIT_SUCESS;
}

/**
 * Switch to private key
 */
itsdk_sigfox_init_t itsdk_sigfox_switchPrivateKey() {
	LOG_INFO_SIGFOXSTK(("itsdk_sigfox_switchPrivateKey\r\n"));
    SIGFOX_API_switch_public_key(false);
	return SIGFOX_INIT_SUCESS;
}


/**
 * Switch to continuous transmission (certification)
 * Give the transmission frequency
 * Give the transmission power
 */
itsdk_sigfox_init_t itsdk_sigfox_continuousModeStart(
		uint32_t				frequency,
		itdsk_sigfox_speed_t 	speed,
		int8_t 					power
) {
	LOG_INFO_SIGFOXSTK(("itsdk_sigfox_continuousModeStart\r\n"));

	if ( power == SIGFOX_POWER_DEFAULT ) power = itsdk_state.sigfox.current_power;
	if ( speed == SIGFOX_SPEED_DEFAULT ) speed = _itsdk_sigfox_getSpeed();
	itsdk_sigfox_setTxPower(power);

	switch (speed) {
		case SIGFOX_SPEED_100:
			SIGFOX_API_start_continuous_transmission (frequency, SFX_DBPSK_100BPS);
			break;
		case SIGFOX_SPEED_600:
			SIGFOX_API_start_continuous_transmission (frequency, SFX_DBPSK_600BPS);
			break;
		default:
			return SIGFOX_INIT_PARAMSERR;
	}
	return SIGFOX_INIT_SUCESS;
}

/**
 * Stop continuous transmission (certification)
 */
itsdk_sigfox_init_t itsdk_sigfox_continuousModeStop() {
	LOG_INFO_SIGFOXSTK(("itsdk_sigfox_continuousModeStop\r\n"));
	SIGFOX_API_stop_continuous_transmission();
	return SIGFOX_INIT_SUCESS;
}


/**
 * Change the RC Sync Period (sigfox payload encryption counter synchronization)
 * The default value is every 4096 frame when the seqId is back to 0
 * The given value is the number of frames
 */
itsdk_sigfox_init_t itsdk_sigfox_setRcSyncPeriod(uint16_t numOfFrame) {
	LOG_INFO_SIGFOXSTK(("itsdk_sigfox_setRcSyncPeriod\r\n"));
	if ( numOfFrame > 4096 ) return SIGFOX_INIT_PARAMSERR;
	SIGFOX_API_set_rc_sync_period(numOfFrame);
	return SIGFOX_INIT_SUCESS;

}


/**
 * Get the Sigfox lib version in use
 * A string is returned terminated by \0
 */
itsdk_sigfox_init_t itsdk_sigfox_getSigfoxLibVersion(uint8_t ** version){
	sfx_u8 __size;
	SIGFOX_API_get_version(version, &__size, VERSION_SIGFOX);
	return SIGFOX_INIT_SUCESS;
}

// ================================================================================
// MANAGE THE NVM STORAGE FOR SIGFOX LIBS
// ================================================================================


#if ITSDK_SIGFOX_NVM_SOURCE == __SFX_NVM_LOCALEPROM

/**
 * Return the size of the sigfox Nvm memory to reserve
 */
itsdk_sigfox_init_t itsdk_sigfox_getNvmSize(uint32_t * sz) {
	*sz = (   sizeof(itsdk_sigfox_nvm_header_t)
			+ itdt_align_32b(SFX_NVMEM_BLOCK_SIZE)
			+ itdt_align_32b(SFX_SE_NVMEM_BLOCK_SIZE)
		  );
	return SIGFOX_INIT_SUCESS;
}

/**
 * Return the offset of the NVM area for Sigfox
 */
itsdk_sigfox_init_t itsdk_sigfox_getNvmOffset(uint32_t * offset) {
	itsdk_sigfox_getSigfoxNvmOffset(offset);
	*offset += sizeof(itsdk_sigfox_nvm_header_t);
	return SIGFOX_INIT_SUCESS;
}

/**
 * Return the offset of the NVM area for Sigfox Secure Element
 */
itsdk_sigfox_init_t itsdk_sigfox_getSeNvmOffset(uint32_t * offset) {
	itsdk_sigfox_getNvmOffset(offset);
	int size = itdt_align_32b(SFX_NVMEM_BLOCK_SIZE);
	*offset += size;
	return SIGFOX_INIT_SUCESS;
}

/**
 * Return the offset of the NVM area for Sigfox
 * Data including the Lib Nvm Offset followed by the
 * the SE offset
 */
itsdk_sigfox_init_t itsdk_sigfox_getSigfoxNvmOffset(uint32_t * offset) {

	uint32_t sstore=0, ssError=0;
	#if ITSDK_WITH_SECURESTORE == __ENABLE
	itsdk_secstore_getStoreSize(&sstore);
	#endif
	#if (ITSDK_WITH_ERROR_RPT == __ENABLE) && (ITSDK_ERROR_USE_EPROM == __ENABLE)
	itsdk_error_getSize(&ssError);
	#endif
	*offset = sstore + ssError;
	return SIGFOX_INIT_SUCESS;
}


/**
 * Configure the default values for the NVM Areas
 */
itsdk_sigfox_init_t __itsdk_sigfox_resetNvmToFactory(bool force) {
	LOG_INFO_SIGFOXSTK(("__itsdk_sigfox_resetNvmToFactory"));

	uint32_t offset;
	itsdk_sigfox_getSigfoxNvmOffset(&offset);

	itsdk_sigfox_nvm_header_t header;
	_eeprom_read(ITDT_EEPROM_BANK0, offset, (void *) &header, sizeof(itsdk_sigfox_nvm_header_t));
	uint32_t expecteSize;
	itsdk_sigfox_getNvmSize(&expecteSize);
	if ( force || header.magic != ITSDK_SIGFOX_NVM_MAGIC || header.size != expecteSize ) {
		LOG_INFO_SIGFOXSTK((".. Reset\r\n"));
		header.magic = ITSDK_SIGFOX_NVM_MAGIC;
		header.size = expecteSize;
		header.reserved = 0;
		_eeprom_write(ITDT_EEPROM_BANK0, offset, (void *) &header, sizeof(itsdk_sigfox_nvm_header_t));
		// force to reset the Sigfox NVM structure
		uint8_t se_nvm_default[SFX_SE_NVMEM_BLOCK_SIZE] = { 0, 0, 0, 0x0F, 0xFF };
		SE_NVM_set(se_nvm_default);
		uint8_t se_mcu_default[SFX_NVMEM_BLOCK_SIZE];
		bzero(se_mcu_default,SFX_NVMEM_BLOCK_SIZE);
		MCU_API_set_nv_mem(se_mcu_default);
	} else {
		LOG_INFO_SIGFOXSTK((".. Skiped\r\n"));
	}
	return SIGFOX_INIT_SUCESS;
}

#endif

// ===================================================================================
// Region conversion
// ===================================================================================
itsdk_sigfox_init_t itsdk_sigfox_getRczFromRegion(uint32_t region, uint8_t * rcz) {
	switch ( region ) {
	case __LPWAN_REGION_EU868:
	case __LPWAN_REGION_MEA868:
		*rcz = SIGFOX_RCZ1;
		break;
	case __LPWAN_REGION_US915:
	case __LPWAN_REGION_SA915:
		*rcz = SIGFOX_RCZ2;
		break;
	case __LPWAN_REGION_JP923:
		*rcz = SIGFOX_RCZ3C;		// to be verified as RCZ3a sound more relevant
		break;
	case __LPWAN_REGION_AU915:
	case __LPWAN_REGION_SA920:
	case __LPWAN_REGION_AP920:
		*rcz = SIGFOX_RCZ4;
		break;
	case __LPWAN_REGION_KR920:
		*rcz = SIGFOX_RCZ5;
		break;
	case __LPWAN_REGION_IN865:
		//*rcz = SIGFOX_RCZ6;
		*rcz = SIGFOX_UNSUPPORTED;
		break;
	default:
		*rcz = SIGFOX_UNSUPPORTED;
		break;
	}
	if ( *rcz == SIGFOX_UNSUPPORTED ) {
		return SIGFOX_INIT_FAILED;
	}
	return SIGFOX_INIT_SUCESS;
}

// ===================================================================================
// Overloadable functions
// ===================================================================================


/**
 * Get the sigfoxKey as a uint8_t[]
 */
itsdk_sigfox_init_t itsdk_sigfox_getKEY(uint8_t * key) {
	LOG_INFO_SIGFOXSTK(("itsdk_sigfox_getKEY\r\n"));
	__api->getDeviceId(key);
	return SIGFOX_INIT_SUCESS;
}



#endif // ITSDK_WITH_SIGFOX_LIB

