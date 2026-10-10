/*

  Copyright (c) 2022 Ethan Zonca

  This file is part of cangaroo.

  cangaroo is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 2 of the License, or
  (at your option) any later version.

  cangaroo is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with cangaroo.  If not, see <http://www.gnu.org/licenses/>.

*/

#include "SLCANInterface.h"
#include "qapplication.h"
#include "qdebug.h"

#include <core/Backend.h>
#include <core/MeasurementInterface.h>
#include <core/CanMessage.h>

#include <iostream>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <QString>
#include <QStringList>
#include <QProcess>
#include <QtSerialPort/QSerialPort>
#include <QtSerialPort/QSerialPortInfo>
#include <QThread>


// ------------------------------------------------------------------
// SLCANConnection - shared serial connection for all channels
// ------------------------------------------------------------------

SLCANConnection::SLCANConnection(const QString &portName)
  : _portName(portName),
    _serport(NULL),
    _refcount(0),
    _isOffline(false),
    _rxbuf_head(0),
    _rxbuf_tail(0),
    _rx_linbuf_ctr(0)
{
    _chan_open[0] = false;
    _chan_open[1] = false;
    _send_wait_respond[0] = 0;
    _send_wait_respond[1] = 0;
    memset(_rxbuf, 0, sizeof(_rxbuf));
    memset(_rx_linbuf, 0, sizeof(_rx_linbuf));

    for(int ch=0; ch<2; ch++)
    {
        _status[ch].can_state = CanInterface::state_bus_off;
        _status[ch].rx_count = 0;
        _status[ch].rx_errors = 0;
        _status[ch].rx_overruns = 0;
        _status[ch].tx_count = 0;
        _status[ch].tx_errors = 0;
        _status[ch].tx_dropped = 0;
        _readMessage_datetime[ch] = QDateTime::currentDateTime();
    }
}

SLCANConnection::~SLCANConnection()
{
    closePortLocked();
}

const QString &SLCANConnection::portName() const
{
    return _portName;
}

void SLCANConnection::ref()
{
    _refcount++;
}

void SLCANConnection::unref()
{
    _refcount--;
}

int SLCANConnection::refCount() const
{
    return _refcount;
}

bool SLCANConnection::isOffline() const
{
    return _isOffline;
}

QString SLCANConnection::version() const
{
    return _version;
}

const can_status_t &SLCANConnection::status(int channel) const
{
    return _status[channel & 1];
}

bool SLCANConnection::openPort()
{
    QMutexLocker locker(&_mutex);

    if(_isOffline)
        return false;

    // Already opened by another channel interface
    if(_serport != NULL && _serport->isOpen())
        return true;

    if(_serport != NULL)
    {
        delete _serport;
        _serport = NULL;
    }

    _serport = new QSerialPort();

    _serport->setPortName(_portName);
    _serport->setBaudRate(1000000);
    _serport->setDataBits(QSerialPort::Data8);
    _serport->setParity(QSerialPort::NoParity);
    _serport->setStopBits(QSerialPort::OneStop);
    _serport->setFlowControl(QSerialPort::NoFlowControl);
    _serport->setReadBufferSize(2048);

    if (!_serport->open(QIODevice::ReadWrite))
    {
        perror("Serport connect failed!");
        _isOffline = true;
        return false;
    }

    _serport->flush();
    _serport->clear();

    // Reset both CAN channels
    _serport->write("C\r", 2);
    _serport->flush();
    _serport->waitForBytesWritten(100);
    _serport->waitForReadyRead(50);

    _serport->write("c\r", 2);
    _serport->flush();
    _serport->waitForBytesWritten(100);

    // Get Version
    _serport->clear(QSerialPort::Input);
    _serport->write("V\r", 2);
    _serport->flush();
    _serport->waitForBytesWritten(100);
    if(_serport->waitForReadyRead(50))
    {
        qApp->processEvents();

        if(_serport->bytesAvailable())
        {
            QByteArray datas = _serport->readLine();
            _version = QString(datas).trimmed();
        }
    }

    _rxbuf_head = 0;
    _rxbuf_tail = 0;
    _rx_linbuf_ctr = 0;

    return true;
}

void SLCANConnection::sendCommand(const char *data, int len)
{
    QMutexLocker locker(&_mutex);

    if(_serport == NULL || !_serport->isOpen() || _isOffline)
        return;

    _serport->write(data, len);
    _serport->flush();
    _serport->waitForBytesWritten(100);
}

void SLCANConnection::markChannelOpened(int channel)
{
    QMutexLocker locker(&_mutex);

    channel &= 1;
    _chan_open[channel] = true;
    _can_msg_queue[channel].clear();
    _can_msg_tx_queue[channel].clear();
    _rx_pending[channel].clear();
    _send_wait_respond[channel] = 0;
    _readMessage_datetime[channel] = QDateTime::currentDateTime();

    _status[channel].can_state = CanInterface::state_ok;
    _status[channel].rx_count = 0;
    _status[channel].rx_errors = 0;
    _status[channel].rx_overruns = 0;
    _status[channel].tx_count = 0;
    _status[channel].tx_errors = 0;
    _status[channel].tx_dropped = 0;
}

void SLCANConnection::closeChannel(int channel)
{
    QMutexLocker locker(&_mutex);

    channel &= 1;

    if(_serport != NULL && _serport->isOpen() && _chan_open[channel])
    {
        const char *cmd = (channel == 0) ? "C\r" : "c\r";
        _serport->write(cmd, 2);
        _serport->flush();
        _serport->waitForBytesWritten(100);
    }

    _chan_open[channel] = false;
    _can_msg_queue[channel].clear();
    _can_msg_tx_queue[channel].clear();
    _rx_pending[channel].clear();
    _send_wait_respond[channel] = 0;
    _status[channel].can_state = CanInterface::state_bus_off;

    // Close the serial port when no channel is open anymore
    if(!_chan_open[0] && !_chan_open[1])
    {
        closePortLocked();
    }
}

void SLCANConnection::closePortLocked()
{
    if(_serport != NULL)
    {
        if(_serport->isOpen())
        {
            _serport->clear();
            _serport->close();
        }
        delete _serport;
        _serport = NULL;
    }
}

void SLCANConnection::queueFrame(int channel, const can_msg_t &frame, const CanMessage &msg)
{
    QMutexLocker locker(&_mutex);

    channel &= 1;
    _can_msg_queue[channel].append(frame);
    _can_msg_tx_queue[channel].append(msg);
    _can_msg_tx_channel.append(channel);
}

bool SLCANConnection::readChannel(int channel, QList<CanMessage> &msglist)
{
    QMutexLocker locker(&_mutex);

    channel &= 1;

    if(_isOffline)
        return false;

    if(_serport == NULL || !_serport->isOpen())
        return true;

    QDateTime datetime = QDateTime::currentDateTime();
    if(datetime.toMSecsSinceEpoch() - _readMessage_datetime[channel].toMSecsSinceEpoch() > 3000)
    {
        _status[channel].can_state = CanInterface::state_ok;
        _send_wait_respond[channel] = 0;
    }

    // Transmit all items queued for this channel
    while(!_can_msg_queue[channel].isEmpty())
    {
        can_msg_t tmp = _can_msg_queue[channel].front();
        _can_msg_queue[channel].pop_front();

        // Write string to serial device
        if(_serport->write(tmp.buf, tmp.length) == tmp.length)
        {
            _send_wait_respond[channel]++;
            _readMessage_datetime[channel] = QDateTime::currentDateTime();
        }
        else
        {
            _status[channel].tx_errors++;

            if(!_can_msg_tx_queue[channel].isEmpty())
                _can_msg_tx_queue[channel].pop_front();
            if(!_can_msg_tx_channel.isEmpty())
                _can_msg_tx_channel.pop_front();
        }

        _serport->waitForBytesWritten(200);
    }

    // RX doesn't work on windows unless we call this for some reason
    if(_serport->waitForReadyRead(0))
    {
        qApp->processEvents();

        if(_serport->bytesAvailable())
        {
            QByteArray datas = _serport->readAll();

            for(int i=0; i<datas.size(); i++)
            {
                // If incrementing the head would hit the tail, the buffer is full. Reset and discard all data.
                if(((_rxbuf_head + 1) % RXCIRBUF_LEN) == _rxbuf_tail)
                {
                    _rxbuf_head = 0;
                    _rxbuf_tail = 0;
                }
                else
                {
                    _rxbuf[_rxbuf_head] = datas.at(i);
                    _rxbuf_head = (_rxbuf_head + 1) % RXCIRBUF_LEN;
                }
            }
        }
    }

    // Parse all complete lines from the ring buffer
    while(_rxbuf_tail != _rxbuf_head)
    {
        if(_rx_linbuf_ctr <= SLCAN_MTU)
        {
            _rx_linbuf[_rx_linbuf_ctr] = _rxbuf[_rxbuf_tail];
            _rx_linbuf_ctr++;

            // If we have a newline, then we just finished parsing a CAN message.
            if(_rxbuf[_rxbuf_tail] == '\r')
            {
                if(_rx_linbuf_ctr > 1)
                {
                    CanMessage msg;
                    int rx_channel = 0;
                    if(parseLine(msg, rx_channel))
                    {
                        _rx_pending[rx_channel].append(msg);
                        _status[rx_channel].rx_count++;
                    }
                }
                else
                {
                    handleAck(datetime);
                }
                _rx_linbuf_ctr = 0;
            }
            else if(_rxbuf[_rxbuf_tail] == '\x07')
            {
                if(_rx_linbuf_ctr == 1)
                {
                    handleNak(datetime);
                }
                _rx_linbuf_ctr = 0;
            }
        }
        else
        {
            perror("Linbuf full");
            _rx_linbuf_ctr = 0;
        }

        _rxbuf_tail = (_rxbuf_tail + 1) % RXCIRBUF_LEN;
    }

    // Deliver the messages of this channel
    msglist.append(_rx_pending[channel]);
    _rx_pending[channel].clear();

    return true;
}

void SLCANConnection::handleAck(const QDateTime &now)
{
    // The TX attribution list tells us which channel this ack belongs to
    int ch = 0;
    if(!_can_msg_tx_channel.isEmpty())
    {
        ch = _can_msg_tx_channel.front();
        _can_msg_tx_channel.pop_front();
    }

    if(_send_wait_respond[ch])
    {
        if(now.toMSecsSinceEpoch() - _readMessage_datetime[ch].toMSecsSinceEpoch() < 200)
        {
            _status[ch].tx_count ++;
            _status[ch].can_state = CanInterface::state_tx_success;
        }
        _send_wait_respond[ch]--;

        if(!_can_msg_tx_queue[ch].isEmpty())
        {
            if(_status[ch].can_state == CanInterface::state_tx_success)
            {
                CanMessage msgtx;
                msgtx.cloneFrom(_can_msg_tx_queue[ch].front());
                if(msgtx.isShow())
                    _rx_pending[ch].append(msgtx);
            }
            _can_msg_tx_queue[ch].pop_front();
        }
    }
}

void SLCANConnection::handleNak(const QDateTime &now)
{
    int ch = 0;
    if(!_can_msg_tx_channel.isEmpty())
    {
        ch = _can_msg_tx_channel.front();
        _can_msg_tx_channel.pop_front();
    }

    if(_send_wait_respond[ch])
    {
        if(now.toMSecsSinceEpoch() - _readMessage_datetime[ch].toMSecsSinceEpoch() < 200)
        {
            _status[ch].tx_errors ++;
            _status[ch].can_state = CanInterface::state_tx_fail;
        }
        _send_wait_respond[ch]--;

        if(!_can_msg_tx_queue[ch].isEmpty())
            _can_msg_tx_queue[ch].pop_front();
    }
}

bool SLCANConnection::parseLine(CanMessage &msg, int &channel)
{
    // Set timestamp to current time
    qint64 msec = QDateTime::currentMSecsSinceEpoch();
    msg.setTimestamp({
        static_cast<long>(msec / 1000),        // Sekunden
        static_cast<long>((msec % 1000) * 1000) // Mikrosekunden
    });

    // Defaults
    msg.setErrorFrame(0);
    msg.setId(0);
    msg.setRTR(false);
    msg.setFD(false);
    msg.setBRS(false);
    msg.setRX(true);

    // Convert from ASCII (2nd character to end)
    for (int i = 1; i < _rx_linbuf_ctr; i++)
    {
        // Lowercase letters
        if(_rx_linbuf[i] >= 'a')
            _rx_linbuf[i] = _rx_linbuf[i] - 'a' + 10;
        // Uppercase letters
        else if(_rx_linbuf[i] >= 'A')
            _rx_linbuf[i] = _rx_linbuf[i] - 'A' + 10;
        // Numbers
        else
            _rx_linbuf[i] = _rx_linbuf[i] - '0';
    }

    bool is_extended = false;
    bool is_rtr = false;
    channel = 0;

    // Handle each incoming command
    switch(_rx_linbuf[0])
    {
        // Channel 0 data frame command
        case 't':
        {
            is_extended = false;
        }
        break;
        case 'T':
        {
            is_extended = true;
        }
        break;

        // Channel 0 remote frame command
        case 'r':
        {
            is_extended = false;
            is_rtr = true;
        }
        break;
        case 'R':
        {
            is_extended = true;
            is_rtr = true;
        }
        break;

        // Channel 1 data frame command
        case 'x':
        {
            is_extended = false;
            channel = 1;
        }
        break;
        case 'X':
        {
            is_extended = true;
            channel = 1;
        }
        break;

        // Channel 1 remote frame command
        case 'y':
        {
            is_extended = false;
            is_rtr = true;
            channel = 1;
        }
        break;
        case 'Y':
        {
            is_extended = true;
            is_rtr = true;
            channel = 1;
        }
        break;

        // CANFD transmit - no BRS (channel 0 only)
        case 'd':
        {
            is_extended = false;
            msg.setFD(true);
            msg.setBRS(false);
        }
        break;
        case 'D':
        {
            is_extended = true;
            msg.setFD(true);
            msg.setBRS(false);
        }
        break;

        // CANFD transmit - with BRS (channel 0 only)
        case 'b':
        {
            is_extended = false;
            msg.setFD(true);
            msg.setBRS(true);
        }
        break;
        case 'B':
        {
            is_extended = true;
            msg.setFD(true);
            msg.setBRS(true);
        }
        break;

        // Invalid command
        default:
        {
            // Reset buffer
            _rx_linbuf_ctr = 0;
            _rx_linbuf[0] = '\0';
            return false;
        }
    }

    // Start parsing at second byte (skip command byte)
    uint8_t parse_loc = 1;

    // Default to standard id len
    uint8_t id_len = SLCAN_STD_ID_LEN;

    // Update length if message is extended ID
    if(is_extended)
        id_len = SLCAN_EXT_ID_LEN;

    uint32_t id_tmp = 0;

    // Iterate through ID bytes
    while(parse_loc <= id_len)
    {
        id_tmp <<= 4;
        id_tmp += _rx_linbuf[parse_loc++];
    }

    msg.setId(id_tmp);
    msg.setExtended(is_extended);
    msg.setRTR(is_rtr);

    // Attempt to parse DLC and check sanity
    uint8_t dlc_code_raw = _rx_linbuf[parse_loc++];

    // If dlc is too long for an FD frame
    if(msg.isFD() && dlc_code_raw > 0xF)
    {
        return false;
    }
    if(!msg.isFD() && dlc_code_raw > 0x8)
    {
        return false;
    }

    if(dlc_code_raw > 0x8)
    {
        switch(dlc_code_raw)
        {
        case 0x9:
            dlc_code_raw = 12;
            break;
        case 0xA:
            dlc_code_raw = 16;
            break;
        case 0xB:
            dlc_code_raw = 20;
            break;
        case 0xC:
            dlc_code_raw = 24;
            break;
        case 0xD:
            dlc_code_raw = 32;
            break;
        case 0xE:
            dlc_code_raw = 48;
            break;
        case 0xF:
            dlc_code_raw = 64;
            break;
        default:
            dlc_code_raw = 0;
            perror("Invalid length");
            break;
        }
    }

    msg.setLength(dlc_code_raw);

    // Calculate number of bytes we expect in the message
    int8_t bytes_in_msg = dlc_code_raw;

    if(bytes_in_msg < 0)
    {
        perror("Invalid length < 0");
        return false;
    }
    if(bytes_in_msg > 64)
    {
        perror("Invalid length > 64");
        return false;
    }

    // Parse data
    for (uint8_t i = 0; i < bytes_in_msg; i++)
    {
        msg.setByte(i,  (_rx_linbuf[parse_loc] << 4) + _rx_linbuf[parse_loc+1]);
        parse_loc += 2;
    }

    // Reset buffer
    _rx_linbuf_ctr = 0;
    _rx_linbuf[0] = '\0';

    return true;
}


// ------------------------------------------------------------------
// SLCANInterface - one CanInterface per device channel
// ------------------------------------------------------------------

SLCANInterface::SLCANInterface(SLCANDriver *driver, int index, int channel, QString name, bool fd_support, uint32_t manufacturer, SLCANConnection *conn)
  : CanInterface((CanDriver *)driver),
    _manufacturer(manufacturer),
    _idx(index),
    _channel(channel),
    _isOpen(false),
    _conn(conn)
{
    _conn->ref();

    // Display name: distinguish the two channels of a device
    _name = name + (_channel == 0 ? " CH1" : " CH2");

    // Set defaults
    _settings.setBitrate(500000);
    _settings.setSamplePoint(875);

    _config.supports_canfd = fd_support;
    _config.supports_timing = false;

    if(fd_support)
    {
        _settings.setFdBitrate(2000000);
        _settings.setFdSamplePoint(750);
    }

    _readMessage_datetime_run = QDateTime::currentDateTime();
}

SLCANInterface::~SLCANInterface()
{
    _conn->unref();
}

QString SLCANInterface::getDetailsStr() const
{
    if(_manufacturer == CANable)
    {
        if(_config.supports_canfd)
        {
            return tr("CANable with CANFD support");
        }
        else
        {
            return tr("CANable with standard CAN support");
        }
    }
    else if(_manufacturer == WeActStudio)
    {
        if(_config.supports_canfd)
        {
            return tr("WeAct Studio USB2CAN with CANFD support");
        }
        else
        {
            return tr("WeAct Studio USB2CAN with standard CAN support");
        }
    }
    else
    {
        return tr("Not Supported");
    }
}

QString SLCANInterface::getName() const
{
	return _name;
}

void SLCANInterface::setName(QString name)
{
    _name = name + (_channel == 0 ? " CH1" : " CH2");
}

QList<CanTiming> SLCANInterface::getAvailableBitrates()
{
    QList<CanTiming> retval;
    QList<unsigned> bitrates;
    QList<unsigned> bitrates_fd;

    QList<unsigned> samplePoints;
    QList<unsigned> samplePoints_fd;

    if(_manufacturer == CANable)
    {
        bitrates.append({10000, 20000, 50000, 83333, 100000, 125000, 250000, 500000, 800000, 1000000});
        bitrates_fd.append({2000000, 5000000});
        samplePoints.append({875});
        samplePoints_fd.append({750});
    }
    else if(_manufacturer == WeActStudio)
    {
        bitrates.append({5000, 10000, 20000, 33333, 50000, 62500, 75000, 83333, 100000, 125000, 250000, 500000, 800000, 1000000});
        bitrates_fd.append({1000000, 2000000, 3000000, 4000000, 5000000});
        samplePoints.append({875});
        samplePoints_fd.append({750});
    }

    unsigned i=0;
    foreach (unsigned br, bitrates)
    {
        for (unsigned br_fd : bitrates_fd)
        {
            foreach (unsigned sp, samplePoints)
            {
                foreach (unsigned sp_fd, samplePoints_fd)
                {
                    retval << CanTiming(i++, br, br_fd, sp,sp_fd);
                }
            }
        }
    }

    return retval;
}

void SLCANInterface::applyConfig(const MeasurementInterface &mi)
{
    // Save settings for port configuration
    _settings = mi;
}

bool SLCANInterface::readConfig()
{
    return false;
}

bool SLCANInterface::readConfigFromLink(rtnl_link *link)
{
    Q_UNUSED(link);
    return false;
}

bool SLCANInterface::supportsTimingConfiguration()
{
    return _config.supports_timing;
}

bool SLCANInterface::supportsCanFD()
{
    return _config.supports_canfd;
}

bool SLCANInterface::supportsTripleSampling()
{
    return false;
}

unsigned SLCANInterface::getBitrate()
{
    return _settings.bitrate();
}

uint32_t SLCANInterface::getCapabilities()
{
    uint32_t retval = 0;

    if(_manufacturer == CANable)
    {
        retval =
            CanInterface::capability_config_os |
            CanInterface::capability_auto_restart |
            CanInterface::capability_listen_only;
    }
    else if(_manufacturer == WeActStudio)
    {
        retval =
            CanInterface::capability_listen_only |
            CanInterface::capability_custom_bitrate |
            CanInterface::capability_custom_canfd_bitrate;
    }

    if (supportsCanFD())
    {
        retval |= CanInterface::capability_canfd;
    }

    if (supportsTripleSampling())
    {
        retval |= CanInterface::capability_triple_sampling;
    }

    return retval;
}

bool SLCANInterface::updateStatistics()
{
    return false;
}

uint32_t SLCANInterface::getState()
{
    return _conn->status(_channel).can_state;
}

int SLCANInterface::getNumRxFrames()
{
    return (int)_conn->status(_channel).rx_count;
}

int SLCANInterface::getNumRxErrors()
{
    return _conn->status(_channel).rx_errors;
}

int SLCANInterface::getNumTxFrames()
{
    return (int)_conn->status(_channel).tx_count;
}

int SLCANInterface::getNumTxErrors()
{
    return _conn->status(_channel).tx_errors;
}

int SLCANInterface::getNumRxOverruns()
{
    return (int)_conn->status(_channel).rx_overruns;
}

int SLCANInterface::getNumTxDropped()
{
    return (int)_conn->status(_channel).tx_dropped;
}

int SLCANInterface::getIfIndex()
{
    return _idx;
}

QString SLCANInterface::getVersion()
{
    return _conn->version();
}

void SLCANInterface::open()
{
    if(!_conn->openPort())
    {
        _isOpen = false;
        return;
    }

    // Reset this channel (idempotent)
    if(_channel == 0)
        _conn->sendCommand("C\r", 2);
    else
        _conn->sendCommand("c\r", 2);

    // Set the classic CAN bitrate
    const char br_cmd = (_channel == 0) ? 'S' : 's';
    if(_settings.isCustomBitrate())
    {
        QString _custombitrate = QString("%1").arg(_settings.customBitrate(), 6, 16,QLatin1Char('0')).toUpper();
        std::string cmd = std::string(1, br_cmd) + _custombitrate.toStdString() + '\r';
        _conn->sendCommand(cmd.c_str(), cmd.length());
    }
    else
    {
        const char *code = "0";
        switch(_settings.bitrate())
        {
            case 1000000: code = "8"; break;
            case 800000:  code = "7"; break;
            case 500000:  code = "6"; break;
            case 250000:  code = "5"; break;
            case 125000:  code = "4"; break;
            case 100000:  code = "3"; break;
            case 83333:   code = "9"; break;
            case 75000:   code = "A"; break;
            case 62500:   code = "B"; break;
            case 50000:   code = "2"; break;
            case 33333:   code = "C"; break;
            case 20000:   code = "1"; break;
            case 10000:   code = "0"; break;
            case 5000:    code = "D"; break;
            default:      code = "0"; break; // Default to 10k
        }
        std::string cmd = std::string(1, br_cmd) + code + "\r";
        _conn->sendCommand(cmd.c_str(), cmd.length());
    }

    // Set configured BRS rate - channel 0 only, FD capable devices only
    if(_config.supports_canfd && _channel == 0)
    {
        if(_settings.isCustomFdBitrate())
        {
            QString _customfdbitrate = QString("%1").arg(_settings.customFdBitrate(), 6, 16,QLatin1Char('0')).toUpper();
            std::string cmd = "Y" + _customfdbitrate.toStdString() + '\r';
            _conn->sendCommand(cmd.c_str(), cmd.length());
        }
        else
        {
            const char *fd_code = NULL;
            switch(_settings.fdBitrate())
            {
                case 1000000: fd_code = "1"; break;
                case 2000000: fd_code = "2"; break;
                case 3000000: fd_code = "3"; break;
                case 4000000: fd_code = "4"; break;
                case 5000000: fd_code = "5"; break;
                default: fd_code = NULL; break;
            }
            if(fd_code != NULL)
            {
                std::string cmd = std::string("Y") + fd_code + "\r";
                _conn->sendCommand(cmd.c_str(), cmd.length());
            }
        }
    }

    // Set Listen Only Mode
    {
        const char m_cmd = (_channel == 0) ? 'M' : 'm';
        std::string cmd = std::string(1, m_cmd) + (_settings.isListenOnlyMode() ? "1\r" : "0\r");
        _conn->sendCommand(cmd.c_str(), cmd.length());
    }

    // Open the channel
    if(_channel == 0)
        _conn->sendCommand("O\r", 2);
    else
        _conn->sendCommand("o\r", 2);

    _conn->markChannelOpened(_channel);

    _isOpen = true;
}

void SLCANInterface::close()
{
    _isOpen = false;
    _conn->closeChannel(_channel);
}

bool SLCANInterface::isOpen()
{
    return _isOpen;
}

void SLCANInterface::sendMessage(const CanMessage &msg)
{
    // SLCAN_MTU plus null terminator
    can_msg_t can_msg;

    uint8_t msg_idx = 0;

    // Message is FD
    if(msg.isFD())
    {
        if(_channel != 0)
            return; // CANFD frames are only supported on channel 0

        // Add character for frame type
        if(msg.isBRS())
        {
            can_msg.buf[msg_idx] = 'b';
        }
        else
        {
            can_msg.buf[msg_idx] = 'd';
        }
    }
    else
    {
        // Message is not FD
        // Add character for frame type, channel 1 uses x/y frame prefixes
        if (msg.isRTR()) {
            can_msg.buf[msg_idx] = (_channel == 0) ? 'r' : 'y';
        }
        else
        {
            can_msg.buf[msg_idx] = (_channel == 0) ? 't' : 'x';
        }
    }

    // Assume standard identifier
    uint8_t id_len = SLCAN_STD_ID_LEN;
    uint32_t tmp = msg.getId();

    // Check if extended
    if (msg.isExtended())
    {
        // Convert first char to upper case for extended frame
        can_msg.buf[msg_idx] -= 32;
        id_len = SLCAN_EXT_ID_LEN;
    }
    msg_idx++;

    // Add identifier to buffer
    for(uint8_t j = id_len; j > 0; j--)
    {
        // Add nibble to buffer
        can_msg.buf[j] = (tmp & 0xF);
        tmp = tmp >> 4;
        msg_idx++;
    }

    // Sanity check length
    int8_t bytes = msg.getLength();

    if(bytes < 0)
        return;
    if(bytes > 64)
        return;

    // If canfd
    if(bytes > 8)
    {
        switch(bytes)
        {
        case 12:
            bytes = 0x9;
            break;
        case 16:
            bytes = 0xA;
            break;
        case 20:
            bytes = 0xB;
            break;
        case 24:
            bytes = 0xC;
            break;
        case 32:
            bytes = 0xD;
            break;
        case 48:
            bytes = 0xE;
            break;
        case 64:
            bytes = 0xF;
            break;
        }
    }

    // Add DLC to buffer
    can_msg.buf[msg_idx++] = bytes;

    // Add data bytes
    for (uint8_t j = 0; j < msg.getLength(); j++)
    {
        can_msg.buf[msg_idx++] = (msg.getByte(j) >> 4);
        can_msg.buf[msg_idx++] = (msg.getByte(j) & 0x0F);
    }

    // Convert to ASCII (2nd character to end)
    for (uint8_t j = 1; j < msg_idx; j++)
    {
        if (can_msg.buf[j] < 0xA) {
            can_msg.buf[j] += 0x30;
        } else {
            can_msg.buf[j] += 0x37;
        }
    }

    // Add CR for slcan EOL
    can_msg.buf[msg_idx++] = '\r';

    // Ensure null termination
    can_msg.buf[msg_idx] = '\0';

    can_msg.length = msg_idx;

    _conn->queueFrame(_channel, can_msg, msg);
}

bool SLCANInterface::readMessage(QList<CanMessage> &msglist, unsigned int timeout_ms)
{
    Q_UNUSED(timeout_ms);

    // Don't saturate the thread. Read the buffer every 1ms.
    QDateTime datetime = QDateTime::currentDateTime();
    if(datetime.toMSecsSinceEpoch() - _readMessage_datetime_run.toMSecsSinceEpoch() >= 1)
    {
        _readMessage_datetime_run = QDateTime::currentDateTime().addMSecs(1);
    }
    else
    {
        return false;
    }

    QThread().msleep(1);

    bool ret = _conn->readChannel(_channel, msglist);

    // Stamp all messages with this interface id
    for(int i=0; i<msglist.size(); i++)
    {
        msglist[i].setInterfaceId(getId());
    }

    if(!ret)
    {
        // Connection is offline
        if(_isOpen)
            close();
    }

    return ret;
}
