/*

  Copyright (c) 2015, 2016 Hubert Denkmair

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

#pragma once

#include "../CanInterface.h"
#include "qdatetime.h"
#include <core/MeasurementInterface.h>
#include <QtSerialPort/QSerialPort>
#include <QtSerialPort/QSerialPortInfo>
#include <QMutex>
#include <QList>

// Maximum rx buffer len
#define SLCAN_MTU (1 + 8 + 1 + 128 + 1) // canfd 64 frame plus \r plus some padding
#define SLCAN_STD_ID_LEN 3
#define SLCAN_EXT_ID_LEN 8

#define RXCIRBUF_LEN 8192 // Buffer for received serial data, serviced at 1ms intervals

class SLCANDriver;
class CanMessage;

typedef struct {
    bool supports_canfd;
    bool supports_timing;
    uint32_t state;
    uint32_t base_freq;
    uint32_t sample_point;
    uint32_t ctrl_mode;
    uint32_t restart_ms;
} can_config_t;

typedef struct {
    uint32_t can_state;

    uint64_t rx_count;
    int rx_errors;
    uint64_t rx_overruns;

    uint64_t tx_count;
    int tx_errors;
    uint64_t tx_dropped;
} can_status_t;

typedef struct {
    char buf[SLCAN_MTU+1];
    qint64 length;
} can_msg_t;


// Shared serial connection for multi-channel SLCAN devices.
// One SLCANConnection exists per COM port and is shared by all channel
// interfaces of that device (Windows does not allow opening the same COM
// port twice).
// Channel 0 uses the classic SLCAN commands (O/C/S/M, frames t/T/r/R),
// channel 1 uses the lowercase variants (o/c/s/m, frames x/X/y/Y).
class SLCANConnection
{
public:
    SLCANConnection(const QString &portName);
    virtual ~SLCANConnection();

    const QString &portName() const;

    void ref();
    void unref();
    int refCount() const;

    // Open the serial port (only once per port) and query the firmware version
    bool openPort();

    // Write a raw command string to the device
    void sendCommand(const char *data, int len);

    // Mark a channel as opened and reset its runtime state
    void markChannelOpened(int channel);

    // Send the channel close command, reset the channel state and close the
    // serial port when no channel is open anymore
    void closeChannel(int channel);

    // Queue a frame for transmission on the given channel
    void queueFrame(int channel, const can_msg_t &frame, const CanMessage &msg);

    // Flush the TX queue of the channel, pump the serial RX buffer and
    // return all received messages that belong to the given channel.
    // Returns false when the connection is offline.
    bool readChannel(int channel, QList<CanMessage> &msglist);

    const can_status_t &status(int channel) const;

    bool isOffline() const;
    QString version() const;

private:
    bool parseLine(CanMessage &msg, int &channel);
    void handleAck(const QDateTime &now);
    void handleNak(const QDateTime &now);
    void closePortLocked();

    QString _portName;
    QSerialPort *_serport;
    QMutex _mutex;

    int _refcount;
    bool _isOffline;
    QString _version;
    bool _chan_open[2];

    char _rxbuf[RXCIRBUF_LEN];
    uint32_t _rxbuf_head;
    uint32_t _rxbuf_tail;
    char _rx_linbuf[SLCAN_MTU+1];
    int _rx_linbuf_ctr;

    QList<can_msg_t> _can_msg_queue[2];
    QList<CanMessage> _can_msg_tx_queue[2];
    QList<int> _can_msg_tx_channel;
    QList<CanMessage> _rx_pending[2];
    uint32_t _send_wait_respond[2];
    QDateTime _readMessage_datetime[2];
    can_status_t _status[2];
};


class SLCANInterface: public CanInterface {
    Q_OBJECT
public:
    enum {
        CANable,
        WeActStudio,
    };
public:
    SLCANInterface(SLCANDriver *driver, int index, int channel, QString name, bool fd_support, uint32_t manufacturer, SLCANConnection *conn);
    virtual ~SLCANInterface();

    QString getDetailsStr() const;
    virtual QString getName() const;
    void setName(QString name);

    virtual QList<CanTiming> getAvailableBitrates();

    virtual void applyConfig(const MeasurementInterface &mi);
    virtual bool readConfig();
    virtual bool readConfigFromLink(struct rtnl_link *link);

    bool supportsTimingConfiguration();
    bool supportsCanFD();
    bool supportsTripleSampling();

    virtual unsigned getBitrate();
    virtual uint32_t getCapabilities();

	virtual void open();
    virtual void close();
    virtual bool isOpen();

    virtual void sendMessage(const CanMessage &msg);
    virtual bool readMessage(QList<CanMessage> &msglist, unsigned int timeout_ms);

    virtual bool updateStatistics();
    virtual uint32_t getState();
    virtual int getNumRxFrames();
    virtual int getNumRxErrors();
    virtual int getNumRxOverruns();

    virtual int getNumTxFrames();
    virtual int getNumTxErrors();
    virtual int getNumTxDropped();

    virtual QString getVersion();

    int getIfIndex();

private:
    uint32_t _manufacturer;

    int _idx;
    int _channel;
    bool _isOpen;
    SLCANConnection *_conn;
    QString _name;
    MeasurementInterface _settings;

    can_config_t _config;

    QDateTime  _readMessage_datetime_run;
};
