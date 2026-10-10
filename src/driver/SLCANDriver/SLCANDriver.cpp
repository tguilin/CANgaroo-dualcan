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


#include "SLCANDriver.h"
#include "SLCANInterface.h"
#include <core/Backend.h>
#include <driver/GenericCanSetupPage.h>

#ifndef _MSC_VER
#include <unistd.h>
#endif
#include <iostream>

#include <QCoreApplication>
#include <QDebug>
#include <QtSerialPort/QSerialPort>
#include <QtSerialPort/QSerialPortInfo>

SLCANDriver::SLCANDriver(Backend &backend)
  : CanDriver(backend),
    setupPage(new GenericCanSetupPage())
{
    QObject::connect(&backend, SIGNAL(onSetupDialogCreated(SetupDialog&)), setupPage, SLOT(onSetupDialogCreated(SetupDialog&)));
}

SLCANDriver::~SLCANDriver()
{
    deleteAllInterfaces();
    purgeConnections();
}

bool SLCANDriver::update()
{
    deleteAllInterfaces();
    purgeConnections();

    int interface_cnt = 0;

    foreach (const QSerialPortInfo &info, QSerialPortInfo::availablePorts())
    {
        bool is_match = false;
        bool fd_support = false;

        if(info.vendorIdentifier() == 0xad50 && info.productIdentifier() == 0x60C4)
        {
            std::cout << "   ++ CANable 1.0 or similar ST USB CDC device detected" << std::endl;

            _manufacturer = SLCANInterface::CANable;
            is_match = true;
            fd_support = false;
        }
        else if(info.vendorIdentifier() == 0x0403 && info.productIdentifier() == 0x6015)
        {
            std::cout << "   ++ CANable 1.0 or similar FT232 device detected" << std::endl;

            _manufacturer = SLCANInterface::CANable;
            is_match = true;
            fd_support = false;
        }
        else if(info.vendorIdentifier() == 0x16D0 && info.productIdentifier() == 0x117E)
        {
            std::cout << "   ++ CANable 2.0 detected" << std::endl;

            _manufacturer = SLCANInterface::CANable;
            is_match = true;
            fd_support = true;
        }
        else if(info.vendorIdentifier() == 1155 && info.productIdentifier() == 22336 && info.serialNumber().startsWith("AAA"))
        {
            std::cout << "   ++ WeAct Studio USB2CAN detected" << std::endl;
            std::cout << "   ++ " << info.serialNumber().toStdString().c_str() << std::endl;

            _manufacturer = SLCANInterface::WeActStudio;
            is_match = true;
            fd_support = true;
        }

        if(is_match)
        {
            // One shared serial connection per COM port
            SLCANConnection *conn = getConnection(info.portName());
            if(conn == NULL)
            {
                conn = new SLCANConnection(info.portName());
                _connections.append(conn);
            }

            // Create two channel interfaces (CH1/CH2) sharing the connection
            createOrUpdateInterface(interface_cnt++, 0, info.portName(), fd_support, _manufacturer, conn);
            createOrUpdateInterface(interface_cnt++, 1, info.portName(), fd_support, _manufacturer, conn);
        }
    }

    return true;
}

QString SLCANDriver::getName()
{
    return "SLCAN";
}

SLCANConnection *SLCANDriver::getConnection(const QString &portName)
{
    foreach (SLCANConnection *conn, _connections)
    {
        if (conn->portName() == portName)
        {
            return conn;
        }
    }
    return NULL;
}

void SLCANDriver::purgeConnections()
{
    QList<SLCANConnection*> keep;
    foreach (SLCANConnection *conn, _connections)
    {
        if (conn->refCount() <= 0)
        {
            delete conn;
        }
        else
        {
            keep.append(conn);
        }
    }
    _connections = keep;
}

SLCANInterface *SLCANDriver::createOrUpdateInterface(int index, int channel, QString name, bool fd_support, uint32_t manufacturer, SLCANConnection *conn)
{
    foreach (CanInterface *intf, getInterfaces())
    {
        SLCANInterface *scif = dynamic_cast<SLCANInterface*>(intf);
        if (scif->getIfIndex() == index)
        {
			scif->setName(name);
            return scif;
		}
	}

    SLCANInterface *scif = new SLCANInterface(this, index, channel, name, fd_support, manufacturer, conn);
    addInterface(scif);

    return scif;
}
