/****************************************************************************
 *
 *   Copyright (c) 2018 PX4 Development Team. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name PX4 nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/**
 * @file sbf.cpp
 *
 * Septentrio protocol as defined in PPSDK SBF Reference Guide 4.1.8
 *
 * @author Matej Frančeškin <Matej.Franceskin@gmail.com>
 *
*/

#include <mathlib/mathlib.h>
#include "sbf.h"

#define SBF_CONFIG_TIMEOUT    200        // ms, timeout for waiting ACK
#define SBF_PACKET_TIMEOUT    2          // ms, if now data during this delay assume that full update received
#define DISABLE_MSG_INTERVAL  1000000  // us, try to disable message with this interval

/**** Trace macros, disable for production builds */
#define SBF_TRACE_PARSER(...)   {/*GPS_INFO(__VA_ARGS__);*/}    /* decoding progress in parse_char() */
#define SBF_TRACE_RXMSG(...)    {/*GPS_INFO(__VA_ARGS__);*/}    /* Rx msgs in payload_rx_done() */
#define SBF_TRACE_SVINFO(...)   {/*GPS_INFO(__VA_ARGS__);*/}    /* NAV-SVINFO processing (debug use only, will cause rx buffer overflows) */

/**** Warning macros, disable to save memory */
#define SBF_WARN(...)        {GPS_WARN(__VA_ARGS__);}
#define SBF_DEBUG(...)       {/*GPS_WARN(__VA_ARGS__);*/}

GPSDriverSBF::GPSDriverSBF(Interface gpsInterface,
			   GPSCallbackPtr callback, void *callback_user,
			   struct vehicle_gps_position_s *gps_position,
			   struct satellite_info_s *satellite_info,
			   uint8_t dynamic_model)
	: GPSHelper(callback, callback_user)
	, _gps_position(gps_position)
	, _satellite_info(satellite_info)
	, _dynamic_model(dynamic_model)
	, _interface(gpsInterface)
{
	decodeInit();
}

GPSDriverSBF::~GPSDriverSBF()
{
}

int
GPSDriverSBF::configure(unsigned &baudrate, OutputMode output_mode)
{
	unsigned baud_i;

	_configured = false;
	_output_mode = output_mode;
	// try different baudrates
	const unsigned baudrates[] = {9600, 38400, 19200, 57600, 115200, 230400};

	for (baud_i = 0; baud_i < sizeof(baudrates) / sizeof(baudrates[0]); baud_i++) {
		baudrate = baudrates[baud_i];
		setBaudrate(baudrate);

		// flush input and wait for at least 20 ms silence
		decodeInit();
		receive(20);
		decodeInit();

		// Change the baudrate
		char msg[64];
		sprintf(msg, SBF_CONFIG_BAUDRATE, baudrate);

		if (!sendMessageAndWaitForAck(msg, SBF_CONFIG_TIMEOUT, false)) {
			continue;
		}

		if (SBF_TX_CFG_PRT_BAUDRATE != baudrate) {
			setBaudrate(SBF_TX_CFG_PRT_BAUDRATE);
			baudrate = SBF_TX_CFG_PRT_BAUDRATE;
		}

		// at this point we have correct baudrate on both ends


		if (_dynamic_model < 6) {
			sprintf(msg, SBF_CONFIG_RECEIVER_DYNAMICS, "low");

		} else if (_dynamic_model < 7) {
			sprintf(msg, SBF_CONFIG_RECEIVER_DYNAMICS, "moderate");

		} else if (_dynamic_model < 8) {
			sprintf(msg, SBF_CONFIG_RECEIVER_DYNAMICS, "high");

		} else {
			sprintf(msg, SBF_CONFIG_RECEIVER_DYNAMICS, "max");
		}

		if (!sendMessageAndWaitForAck(msg, SBF_CONFIG_TIMEOUT, false)) {
			continue;
		}

		const char *config_cmds = SBF_CONFIG;
		uint8_t i = 0;
		msg[0] = 0;

		while (*config_cmds != 0) {
			msg[i] = *config_cmds;

			if (msg[i++] == '\n') {
				msg[i] = 0;

				if (!sendMessageAndWaitForAck(msg, SBF_CONFIG_TIMEOUT, false)) {
					break;
				}

				i = 0;
				msg[0] = 0;
			}

			config_cmds++;
		}

		break;
	}

	if (baud_i >= sizeof(baudrates) / sizeof(baudrates[0])) {
		return -1;    // connection and/or baudrate detection failed
	}

	_configured = true;

	return 0;
}

bool
GPSDriverSBF::sendMessageAndWaitForAck(const char *msg, const unsigned timeout, const bool report)
{
	// Send message
	int length = strlen(msg);

	if (write(msg, length) != length) {
		return false;
	}

	// Wait for acknowledge
	// For all valid set -, get - and exe -commands, the first line of the reply is an exact copy
	// of the command as entered by the user, preceded with "$R:"
	char buf[GPS_READ_BUFFER_SIZE];
	int ret = read(reinterpret_cast<uint8_t *>(buf), sizeof(buf), SBF_PACKET_TIMEOUT);

	if (ret < 0) {
		// something went wrong when polling or reading
		SBF_WARN("sbf poll_or_read err");
		return false;
	}

	if (ret < length + 4 || strncmp(buf, "$R: ", 4) || strcmp(buf + 4, msg)) {
		return false;
	}

	return true;
}

int    // -1 = error, 0 = no message handled, 1 = message handled, 2 = sat info message handled
GPSDriverSBF::receive(unsigned timeout)
{
	uint8_t buf[GPS_READ_BUFFER_SIZE];

	// timeout additional to poll
	gps_abstime time_started = gps_absolute_time();

	int handled = 0;

	while (true) {
		bool ready_to_return = _configured ? (_got_pos && _got_dop) : handled;

		// Wait for only SBF_PACKET_TIMEOUT if something already received.
		int ret = read(buf, sizeof(buf), ready_to_return ? SBF_PACKET_TIMEOUT : timeout);

		if (ret < 0) {
			// something went wrong when polling or reading
			SBF_WARN("ubx poll_or_read err");
			return -1;

		} else if (ret == 0) {
			// return success if ready
			if (ready_to_return) {
				_got_pos = false;
				_got_dop = false;
				return handled;
			}

		} else {
			//SBF_DEBUG("read %d bytes", ret);

			// pass received bytes to the packet decoder
			for (int i = 0; i < ret; i++) {
				handled |= parseChar(buf[i]);
				//SBF_DEBUG("parsed %d: 0x%x", i, buf[i]);
			}
		}

		// abort after timeout if no useful packets received
		if (time_started + timeout * 1000 < gps_absolute_time()) {
			SBF_DEBUG("timed out, returning");
			return -1;
		}
	}
}

int    // 0 = decoding, 1 = message handled, 2 = sat info message handled
GPSDriverSBF::parseChar(const uint8_t b)
{
	int ret = 0;

	switch (_decode_state) {

	// Expecting Sync1
	case SBF_DECODE_SYNC1:
		if (b == SBF_SYNC1) {    // Sync1 found --> expecting Sync2
			SBF_TRACE_PARSER("A");
			_decode_state = SBF_DECODE_SYNC2;
		}

		break;

	// Expecting Sync2
	case SBF_DECODE_SYNC2:
		if (b == SBF_SYNC2) {    // Sync2 found --> expecting CRC
			SBF_TRACE_PARSER("B");
			_decode_state = SBF_DECODE_CRC1;

		} else {        // Sync1 not followed by Sync2: reset parser
			decodeInit();
		}

		break;

	// Expecting CRC
	case SBF_DECODE_CRC1:
		SBF_TRACE_PARSER("C");
		_crc = b;
		_decode_state = SBF_DECODE_CRC2;
		break;

	case SBF_DECODE_CRC2:
		SBF_TRACE_PARSER("D");
		_crc |= b << 8;
		_decode_state = SBF_DECODE_MSG1;
		break;

	// Expecting MSG
	case SBF_DECODE_MSG1:
		SBF_TRACE_PARSER("E");
		ret = payloadRxAdd(b);        // add a payload byte
		_decode_state = SBF_DECODE_MSG2;
		break;

	case SBF_DECODE_MSG2:
		SBF_TRACE_PARSER("F");
		ret = payloadRxAdd(b);        // add a payload byte
		_decode_state = SBF_DECODE_LENGTH1;
		break;

	// Expecting first length byte
	case SBF_DECODE_LENGTH1:
		SBF_TRACE_PARSER("G");
		ret = payloadRxAdd(b);        // add a payload byte
		_rx_payload_length = b;
		_decode_state = SBF_DECODE_LENGTH2;
		break;

	// Expecting second length byte
	case SBF_DECODE_LENGTH2:
		SBF_TRACE_PARSER("H");
		ret = payloadRxAdd(b);        // add a payload byte
		_rx_payload_length |= b << 8; // calculate payload size
		_rx_payload_length += 4;      // For MSG & Length
		_rx_payload_index = 0;
		_decode_state = SBF_DECODE_PAYLOAD;
		break;

	// Expecting payload
	case SBF_DECODE_PAYLOAD:
		SBF_TRACE_PARSER(".");

		ret = payloadRxAdd(b);        // add a payload byte

		if (ret < 0) {
			// payload not handled, discard message
			decodeInit();

		} else if (ret > 0) {
			ret = payloadRxDone();    // finish payload processing
			decodeInit();

		} else {
			// expecting more payload, stay in state SBF_DECODE_PAYLOAD
		}

		ret = 0;
		break;

	default:
		break;
	}

	return ret;
}

/**
 * Add payload rx byte
 */
int    // -1 = error, 0 = ok, 1 = payload completed
GPSDriverSBF::payloadRxAdd(const uint8_t b)
{
	int ret = 0;
	uint8_t *p_buf = (uint8_t *)&_buf;

	p_buf[_rx_payload_index] = b;

	if (++_rx_payload_index >= _rx_payload_length) {
		ret = 1;    // payload received completely
	}

	return ret;
}

/**
 * Calculate buffer CRC16
 */
uint16_t
crc16(uint8_t *data_p, uint16_t length)
{
	uint8_t x;
	uint16_t crc = 0;

	while (length--) {
		x = crc >> 8 ^ *data_p++;
		x ^= x >> 4;
		crc = (crc << 8) ^ ((uint16_t)(x << 12)) ^ ((uint16_t)(x << 5)) ^ ((uint16_t)x);
	}

	return crc;
}

/**
 * Finish payload rx
 */
int    // 0 = no message handled, 1 = message handled, 2 = sat info message handled
GPSDriverSBF::payloadRxDone()
{
	int ret = 0;

	if (_crc != crc16((uint8_t *) &_buf, _rx_payload_length)) {
		return 1;
	}

	// handle message
	switch (_buf.msg_id) {
	case SBF_ID_PVTGeodetic: {
		SBF_TRACE_RXMSG("Rx PVTGeodetic");
		_msg_status |= 1;

		if (_buf.payload_pvt_geodetic.mode_type < 1) {
			_gps_position->fix_type = 1;

		} else if (_buf.payload_pvt_geodetic.mode_type == 6) {
			_gps_position->fix_type = 4;

		} else if (_buf.payload_pvt_geodetic.mode_type == 5 || _buf.payload_pvt_geodetic.mode_type == 8) {
			_gps_position->fix_type = 5;

		} else if (_buf.payload_pvt_geodetic.mode_type == 4 || _buf.payload_pvt_geodetic.mode_type == 7) {
			_gps_position->fix_type = 6;

		} else {
			_gps_position->fix_type = 3;
		}

		_gps_position->vel_ned_valid = _gps_position->fix_type > 1 && _buf.payload_pvt_geodetic.error == 0;
		_gps_position->satellites_used = _buf.payload_pvt_geodetic.nr_sv;

		_gps_position->lat = (float)_buf.payload_pvt_geodetic.latitude * M_RAD_TO_DEG_F;
		_gps_position->lon = (float)_buf.payload_pvt_geodetic.longitude * M_RAD_TO_DEG_F;
		_gps_position->alt_ellipsoid = _buf.payload_pvt_geodetic.height;
		_gps_position->alt = _gps_position->alt_ellipsoid - _buf.payload_pvt_geodetic.undulation;

		_gps_position->eph = (float)_buf.payload_pvt_geodetic.h_accuracy / 100.0f;
		_gps_position->epv = (float)_buf.payload_pvt_geodetic.v_accuracy / 100.0f;

		_gps_position->vel_n_m_s = (float)_buf.payload_pvt_geodetic.vn;
		_gps_position->vel_e_m_s = (float)_buf.payload_pvt_geodetic.ve;
		_gps_position->vel_d_m_s = -(float)_buf.payload_pvt_geodetic.vu;
		_gps_position->vel_m_s = sqrtf(_gps_position->vel_n_m_s * _gps_position->vel_n_m_s + _gps_position->vel_e_m_s * _gps_position->vel_e_m_s);

		_gps_position->cog_rad = (float)_buf.payload_pvt_geodetic.cog * M_DEG_TO_RAD_F;
		_gps_position->c_variance_rad = (float)1 * M_DEG_TO_RAD_F * 1e-5f;

		_gps_position->time_utc_usec = 0;
#ifndef NO_MKTIME
			/* convert to unix timestamp */
			struct tm timeinfo;
			memset(&timeinfo, 0, sizeof(timeinfo));

			timeinfo.tm_year = 1980 - 1900;
			timeinfo.tm_mon = 0;
			timeinfo.tm_mday = 6 + _buf.WNc * 7;
			timeinfo.tm_hour = 0;
			timeinfo.tm_min = 0;
			timeinfo.tm_sec = _buf.TOW / 1000;

			time_t epoch = mktime(&timeinfo);

			if (epoch > GPS_EPOCH_SECS) {
				// FMUv2+ boards have a hardware RTC, but GPS helps us to configure it
				// and control its drift. Since we rely on the HRT for our monotonic
				// clock, updating it from time to time is safe.

				timespec ts;
				memset(&ts, 0, sizeof(ts));
				ts.tv_sec = epoch;
				ts.tv_nsec = (_buf.TOW % 1000) * 1000 * 1000;
				setClock(ts);

				_gps_position->time_utc_usec = static_cast<uint64_t>(epoch) * 1000000ULL;
				_gps_position->time_utc_usec += (_buf.TOW % 1000) * 1000;
			}

#endif

			_gps_position->timestamp = gps_absolute_time();
			_last_timestamp_time = _gps_position->timestamp;
			_rate_count_vel++;
			_rate_count_lat_lon++;
			_got_pos = true;
			ret = (_msg_status == 7) ? 1 : 0;
			break;
		}

	case SBF_ID_VelCovGeodetic:
		SBF_TRACE_RXMSG("Rx VelCovGeodetic");
		_msg_status |= 2;
		_gps_position->s_variance_m_s = math::max(math::max(_buf.payload_vel_col_geodetic.cov_ve_ve,
						_buf.payload_vel_col_geodetic.cov_vn_vn), _buf.payload_vel_col_geodetic.cov_vu_vu);
		break;

	case SBF_ID_DOP:
		SBF_TRACE_RXMSG("Rx DOP");
		_msg_status |= 4;
		_gps_position->hdop = _buf.payload_dop.hDOP * 0.01f;
		_gps_position->vdop = _buf.payload_dop.vDOP * 0.01f;
		_got_dop = true;
		break;

	case SBF_ID_ChannelStatus: {
			SBF_TRACE_RXMSG("Rx SBF_ID_ChannelStatus");
			_satellite_info->timestamp = gps_absolute_time();
			_satellite_info->count = _buf.payload_channel_status.n;
			uint8_t *buf_ptr = reinterpret_cast<uint8_t *>(&_buf.payload_channel_status.satinfo);

			for (uint8_t i = 0; i < _satellite_info->count && i < _satellite_info->SAT_INFO_MAX_SATELLITES; i++) {
				sbf_payload_channel_sat_info_t *sat_info = reinterpret_cast<sbf_payload_channel_sat_info_t *>(buf_ptr);
				_satellite_info->svid[i] = sat_info->svid;
				_satellite_info->used[i] = (sat_info->health_status == 1) ? 1 : 0;
				_satellite_info->elevation[i] = sat_info->elevation;
				_satellite_info->azimuth[i] = sat_info->azimuth & ((1 << 9) - 1);
				_satellite_info->snr[i] = 0;
				buf_ptr += _buf.payload_channel_status.sb1_length + sat_info->n2 * _buf.payload_channel_status.sb2_length;
			}

			ret = 2;
			break;
		}

	default:
		break;
	}

	if (ret > 0) {
		_gps_position->timestamp_time_relative = (int32_t)(_last_timestamp_time - _gps_position->timestamp);
	}

	return ret;
}

void
GPSDriverSBF::decodeInit()
{
	_decode_state = SBF_DECODE_SYNC1;
	_crc = 0;
	_rx_payload_length = 0;
	_rx_payload_index = 0;
}