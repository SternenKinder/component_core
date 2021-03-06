/*
 * Ubitrack - Library for Ubiquitous Tracking
 * Copyright 2006, Technische Universitaet Muenchen, and individual
 * contributors as indicated by the @authors tag. See the
 * copyright.txt in the distribution for a full listing of individual
 * contributors.
 *
 * This is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation; either version 2.1 of
 * the License, or (at your option) any later version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this software; if not, write to the Free
 * Software Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA, or see the FSF site: http://www.fsf.org.
 */


/**
 * @ingroup driver_components
 * @file
 * Network Source
 * This file contains the driver component to
 * receive measurements through a network connection.
 *
 * The driver is built as a module to handle
 * the socket communication for each component.
 *
 * The received data is sent via a push interface.
 *
 * @author Florian Echtler <echtler@in.tum.de>
 */

#ifndef _NETWORKSOURCE_H_
#define _NETWORKSOURCE_H_

#include <string>
#include <cstdlib>

// on windows, asio must be included before anything that possible includes windows.h
// don't ask why.
#include <boost/asio.hpp>

#include <utDataflow/PushSupplier.h>
#include <utDataflow/Component.h>
#include <utDataflow/Module.h>
#include <utMeasurement/Measurement.h>
#include <utMeasurement/TimestampSync.h>
#include <utUtil/TracingProvider.h>

// have a logger..
static log4cpp::Category& logger( log4cpp::Category::getInstance( "Drivers.NetworkSource" ) );

namespace Ubitrack { namespace Drivers {

using namespace Dataflow;

// forward declaration
class SourceComponentBase;

/**
 * Module key for network source.
 * Represents the port number on which to listen.
 */
class SourceModuleKey
	: public DataflowConfigurationAttributeKey< int >
{
public:
	SourceModuleKey( boost::shared_ptr< Graph::UTQLSubgraph > subgraph )
        : DataflowConfigurationAttributeKey< int >( subgraph, "networkPort", 0x5554 ) // default port is 0x5554 (UT)
    { }
};


/**
 * Component key for network source.
 * Contains either the subgraph id or the value of the "senderId" dataflow attribute (if present)
 */
class SourceComponentKey
	: public std::string
{
public:
	SourceComponentKey( const std::string& s )
		: std::string( s )
	{}

	SourceComponentKey( boost::shared_ptr< Graph::UTQLSubgraph > subgraph )
    {
		if ( !subgraph->m_DataflowAttributes.hasAttribute( "senderId" ) )
			assign( subgraph->m_ID );
		else
			assign( subgraph->m_DataflowAttributes.getAttributeString( "senderId" ) );
	}
};


/**
 * Module for network source.
 * Does all the dirty asio work.
 */
class SourceModule
	: public Module< SourceModuleKey, SourceComponentKey, SourceModule, SourceComponentBase >
{
public:

	/** constructor */
	SourceModule( const SourceModuleKey& key, boost::shared_ptr< Graph::UTQLSubgraph > subgraph, FactoryHelper* pFactory );

	/** destruktor */
	~SourceModule();

	/** thread method */
	void HandleReceive (const boost::system::error_code err, size_t length);

	boost::shared_ptr< SourceComponentBase > createComponent( const std::string& type, const std::string& name,
		boost::shared_ptr< Graph::UTQLSubgraph > subgraph, const ComponentKey& key, ModuleClass* pModule );

	/** module stop method */
	virtual void startModule();

	/** module start method */
	virtual void stopModule();

protected:

	boost::shared_ptr< boost::asio::io_service > m_IoService;
	boost::shared_ptr< boost::asio::ip::udp::socket > m_Socket;

	// Recive data. Do not touch from outside of async network thread
	enum { max_receive_length = 10240, receive_buffer_size = 10242 };	
	char receive_data[receive_buffer_size];
	boost::asio::ip::udp::endpoint sender_endpoint;

	boost::shared_ptr< boost::thread > m_NetworkThread;

};


/**
 * Virtual base class for all other components
 */
class SourceComponentBase
	: public SourceModule::Component
{
public:

	/** constructor */
	SourceComponentBase( const std::string& name, boost::shared_ptr< Graph::UTQLSubgraph > subgraph, const SourceComponentKey& componentKey, SourceModule* pModule )
		: SourceModule::Component( name, componentKey, pModule )
	{}

	virtual ~SourceComponentBase()
	{}

	virtual void parse( boost::archive::text_iarchive& ar, Measurement::Timestamp recvtime )
	{}

};


template< class EventType >
class SourceComponent
	: public SourceComponentBase
{

public:

	SourceComponent( const std::string& name, boost::shared_ptr< Graph::UTQLSubgraph > subgraph, const SourceComponentKey& key, SourceModule* module )
		: SourceComponentBase( name, subgraph, key, module )
		, m_port( "Output", *this )
		, m_synchronizer( 1e9 )
		, m_firstTimestamp( 0 )
	{}

	void parse( boost::archive::text_iarchive& ar, Measurement::Timestamp recvtime )
	{
		EventType mm( boost::shared_ptr< typename EventType::value_type >( new typename EventType::value_type() ) );
		Measurement::Timestamp sendtime;
		ar >> mm;
		ar >> sendtime;

		LOG4CPP_DEBUG( logger, "perceived host clock offset: " << static_cast< long long >( recvtime - sendtime ) * 1e-6 << "ms" );

		// subtract first timestamp to avoid losing timing precision
		if ( !m_firstTimestamp )
			m_firstTimestamp = sendtime;

		// synchronize sender time with receiver time
		Measurement::Timestamp correctedTime = m_synchronizer.convertNativeToLocal( sendtime - double( m_firstTimestamp ), recvtime );

		// add offset of individual measurements
		correctedTime -= static_cast< long long >( sendtime - mm.time() );

		LOG4CPP_DEBUG( logger, "Timestamps measurement: " << Measurement::timestampToShortString( mm.time() )
			<< ", sent: " << Measurement::timestampToShortString( sendtime )
			<< ", arrival: " << Measurement::timestampToShortString( recvtime )
			<< ", corrected: " << Measurement::timestampToShortString( correctedTime ) );

#ifdef ENABLE_EVENT_TRACING
		TRACEPOINT_MEASUREMENT_CREATE(getEventDomain(), correctedTime, getName().c_str(), "NetworkSource")
#endif
		m_port.send( EventType( correctedTime, mm ) );
	}

protected:
	PushSupplier< EventType > m_port;
	Measurement::TimestampSync m_synchronizer;
	Measurement::Timestamp m_firstTimestamp;
};




} } // namespace Ubitrack::Drivers

#endif
