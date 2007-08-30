// -*- mode: c++; mode: auto-fill; mode: flyspell-prog; -*-
// Author Antonio Bulgheroni, INFN <mailto:antonio.bulgheroni@gmail.com>
// Version $Id: EUTelMimoTelReader.cc,v 1.10 2007-08-30 09:00:28 bulgheroni Exp $
/*
 *   This source code is part of the Eutelescope package of Marlin.
 *   You are free to use this source files for your own development as
 *   long as it stays in a public research context. You are not
 *   allowed to use it for commercial purpose. You must put this
 *   header with author names in all development based on this file.
 *
 */

#ifdef USE_EUDAQ
// personal includes
#include "EUTelMimoTelReader.h"
#include "EUTelRunHeaderImpl.h"
#include "EUTelEventImpl.h"
#include "EUTelExceptions.h"
#include "EUTelSparseDataImpl.h"
#include "EUTelSimpleSparsePixel.h"

// marlin includes
#include "marlin/Exceptions.h"
#include "marlin/Processor.h"
#include "marlin/DataSourceProcessor.h"
#include "marlin/ProcessorMgr.h"

// eudaq includes 
#include <eudaq/FileSerializer.hh>
#include <eudaq/EUDRBEvent.hh>
#include <eudaq/DetectorEvent.hh>
#include <eudaq/Utils.hh>
#include <eudaq/Exception.hh>

// lcio includes
#include <IMPL/LCCollectionVec.h>
#include <IMPL/TrackerRawDataImpl.h>
#include <IMPL/TrackerDataImpl.h>
#include <IMPL/LCEventImpl.h>
#include <UTIL/CellIDEncoder.h>

// system includes 
#include <string>
#include <vector>
#include <algorithm>
#include <memory>
#include <iostream>
#include <iomanip>

using namespace std;
using namespace marlin;
using namespace eutelescope;
using namespace eudaq;

EUTelMimoTelReader::EUTelMimoTelReader (): DataSourceProcessor  ("EUTelMimoTelReader") {
  
  _description =
    "Reads EUDRB events with MimoTel data inside";
 
  registerOptionalParameter("CDS","Enable (==1) or disable (==0) the CDS calculation",
			  _cdsCalculation, static_cast< bool > ( true ) );

  registerOutputCollection(LCIO::TRACKERRAWDATA,"FirstFrameCollectionName",
			   "First frame collection name", _firstFrameCollectionName, 
			   string( "firstFrame" ));
  registerOutputCollection(LCIO::TRACKERRAWDATA,"SecondFrameCollectionName",
			   "Second frame collection name", _secondFrameCollectionName, 
			   string( "secondFrame" ));
  registerOutputCollection(LCIO::TRACKERRAWDATA,"ThirdFrameCollectionName",
			   "Third frame collection name", _thirdFrameCollectionName, 
			   string( "thirdFrame" ));
  registerOutputCollection(LCIO::TRACKERRAWDATA,"CDSCollection",
			   "CDS collection name", _cdsCollectionName, string( "rawdata" ));

  registerOutputCollection(LCIO::TRACKERDATA,"ZSCollection",
			   "ZS collection name", _zsFrameCollectionName, string( "zsdata" ));
  
  registerProcessorParameter("InputDataFileName", "Inpuf file",
			     _fileName, string("run012345.raw") );
  
  registerProcessorParameter("SignalPolarity", "Signal polarity (negative == -1)",
			     _polarity, static_cast<int> (-1));

  registerProcessorParameter("GeoID", "The geometry identification number",
			     _geoID, static_cast<int> ( 0 ));

  registerProcessorParameter("RemoveMarker","If set to true, pixels identified as markers will be removed\n"
			     "from the TrackerRawData output collections",
			     _removeMarkerSwitch, static_cast< bool > ( false ) );

  IntVec markerPositionExample;
  markerPositionExample.push_back( 0  );
  markerPositionExample.push_back( 1  );
  markerPositionExample.push_back( 66 );
  markerPositionExample.push_back( 67 );
  markerPositionExample.push_back( 132 );
  markerPositionExample.push_back( 133 );
  markerPositionExample.push_back( 198 );
  markerPositionExample.push_back( 199 );

  registerOptionalParameter("MarkerPosition","This vector of integer contains the marker positions in pixel number.\n"
			    "(Pixels are numbered starting from 0)",
			    _markerPositionVec, markerPositionExample );
			    
  registerOptionalParameter("SparsePixelType", "Type of sparsified pixel data structure (use SparsePixelType enum)",
			    _pixelType , static_cast<int> ( 1 ) );


}

EUTelMimoTelReader * EUTelMimoTelReader::newProcessor () {
  return new EUTelMimoTelReader;
}

void EUTelMimoTelReader::init () {
  printParameters ();

  // those two values are constants!!!
  _xMax = 264;
  _yMax = 256;

  if ( _removeMarkerSwitch ) {

    streamlog_out( DEBUG4 ) << "Data conversion with markers removal" << endl;
    if ( _markerPositionVec.size() == 0 ) {
      streamlog_out( WARNING2 ) <<  "Data conversion with markers removal selected but no markers position found" << endl
			       <<  "Disabling marker removal" << endl;
      _removeMarkerSwitch = false;
      sort( _markerPositionVec.begin(), _markerPositionVec.end() );
    }
  } else {
    _markerPositionVec.clear();
  }
}


void EUTelMimoTelReader::readDataSource (int numEvents) {

  // this event counter is used to stop the processing when it is
  // greater than numEvents.
  int eventCounter = 0;
  
  streamlog::logscope scope(streamlog::out);
  scope.setName(name());

  streamlog_out( DEBUG4 ) << "Reading " << _fileName << " with eudaq file deserializer " << endl;

  // this is eudaq de-serialiazer for the input file 
  FileDeserializer des( _fileName );
  EUDRBDecoder * eudrbDecoder = NULL;
  
  // the EUDRB can work in different modalities. These are specified
  // in the BORE event using two flags: the eudrbGlobalMode is
  // defining a global mode of operation for all boards in the data
  // producer, while the eudrbSubMode is specifying each board working
  // mode in case they are different. 
  string           eudrbGlobalMode ;
  vector<string >  eudrbSubMode;

  while ( des.HasData() ) {

    counted_ptr<Event> ev(EventFactory::Create(des));

    // increment the event counter here. This number is not used to
    // set the event number but only to count how many events have
    // been processed to stop the conversion
    ++eventCounter;
    if ( eventCounter >=  numEvents ) {
      // even if there are some more events in the input file, we
      // don't want to decode them. So add (eventually another kEORE)
      // and then stop the data reading
      EUTelEventImpl * event = new EUTelEventImpl;
      event->setEventType(kEORE);
      event->setTimeStamp  ( ev->GetTimestamp()   );
      event->setRunNumber  ( ev->GetRunNumber()   );
      event->setEventNumber( ev->GetEventNumber() );
      ProcessorMgr::instance()->processEvent( static_cast<LCEventImpl*> (event) ) ;
      delete event;
      break;
    }

    // dev is a detector event. The detector event is a collection of
    // all event types in the data stream. There should be an event
    // for the TLU, one for the EUDRB and one for all the other DataProducer.
    DetectorEvent * dev = dynamic_cast<DetectorEvent*>(ev.get());

    if ( ev->IsBORE() ) {
      // that's the right place to process the run header
      streamlog_out( DEBUG4 ) << "Found a BORE, so processing the RunHeader" << endl;
      auto_ptr<IMPL::LCRunHeaderImpl> lcHeader( new IMPL::LCRunHeaderImpl );
      auto_ptr<EUTelRunHeaderImpl>    runHeader( new EUTelRunHeaderImpl( lcHeader.get() ) );

      runHeader->lcRunHeader()->setRunNumber( ev->GetRunNumber() );
      
      eudrbDecoder = new EUDRBDecoder(*dev);      
      int noOfDetectors = 0;
      
      // now loop over all the detector sub-event. Of course we are
      // particularly interested in the case the sub-event is a EUDRB
      // event.
      for (unsigned int i = 0; i < dev->NumEvents(); ++i) {
	Event * subev = dev->GetEvent(i);

	// try to convert the subevent into a EUDRBEvent. 
	EUDRBEvent * eudev = dynamic_cast<EUDRBEvent*>(subev);
	
	// if the dynamic_cast succeeded then the event is a EUDRB event
	if (eudev) {
	  noOfDetectors += from_string(eudev->GetTag("BOARDS"), 0) ;
	  eudrbGlobalMode   = eudev->GetTag("MODE");
	  string det  = eudev->GetTag("DET");
	  if      ( eudrbGlobalMode == "RAW3" )   streamlog_out( MESSAGE2 ) << "All boards are working in RAW3 mode" << endl;
	  else if ( eudrbGlobalMode == "ZS" )     streamlog_out( MESSAGE2 ) << "All boards are working in ZS mode" << endl;
	  else if ( eudrbGlobalMode == "Mixed" )  {
	    streamlog_out( MESSAGE2 ) << "Boards running in mixed mode" << endl;
	    streamlog_out( DEBUG4  ) << "Discovering the mode of operation of each board" << endl;
	    for ( int iBoard = 0;  iBoard < from_string(eudev->GetTag("BOARDS"), 0); iBoard++ ) {
	      stringstream ss;
	      ss << "MODE" << iBoard;
	      string subMode = eudev->GetTag( ss.str().c_str() );
	      eudrbSubMode.push_back( subMode ); 
	      streamlog_out( DEBUG4 ) << "Board " << iBoard << " working in " << subMode << endl;
	    }
	  } else throw InvalidParameterException("For the time being only RAW3 is supported");
	  runHeader->setEUDRBMode( eudrbGlobalMode );
	}
      }

      runHeader->setNoOfDetector(noOfDetectors);
      runHeader->setMinX(IntVec(noOfDetectors, 0));
      runHeader->setMaxX(IntVec(noOfDetectors, _xMax - _markerPositionVec.size() -  1));
      runHeader->setMinY(IntVec(noOfDetectors, 0));
      runHeader->setMaxY(IntVec(noOfDetectors, _yMax - 1));
      runHeader->lcRunHeader()->setDetectorName("MimoTel");
      runHeader->setGeoID( _geoID );
      runHeader->setDateTime();
      runHeader->addProcessor( type() );


      ProcessorMgr::instance()->processRunHeader(lcHeader.release());
      delete lcHeader.get();

    } else if ( ev->IsEORE() ) {
      streamlog_out( DEBUG4 ) << "Found a EORE, processing a dummy empty event" << endl; 
      EUTelEventImpl * event = new EUTelEventImpl;
      event->setEventType(kEORE);
      event->setTimeStamp( ev->GetTimestamp() );
      event->setRunNumber( ev->GetRunNumber() );
      event->setEventNumber( ev->GetEventNumber() );
      ProcessorMgr::instance()->processEvent( static_cast<LCEventImpl*> (event) ) ;
      delete event;
    } else if (!dev) {
      streamlog_out( WARNING2 ) << "Event number " << ev->GetEventNumber() << " does not contain any data " << endl;
      throw SkipEventException(this);
    } else {
      // this is the last case, this is a kDE event no chance to fail!
      // This part of the code is enclosed into a try/catch block,
      // because the event has to be added to the event only if it is
      // properly decoded.

      try {
	// to avoid any memory leak, in this try/catch block event is
	// owned by auto_ptr
	auto_ptr<EUTelEventImpl> event(new EUTelEventImpl);
	event->setRunNumber( ev->GetRunNumber() );
	event->setEventNumber( ev->GetEventNumber() );
	event->setTimeStamp( ev->GetTimestamp() );
	event->setEventType( kDE );
	
	// prepare a collection for each frame and set up the cell ID
	// encoder for each of them. 
	auto_ptr<LCCollectionVec> firstFrameColl ( new LCCollectionVec (LCIO::TRACKERRAWDATA) );
	auto_ptr<LCCollectionVec> secondFrameColl( new LCCollectionVec (LCIO::TRACKERRAWDATA) );
	auto_ptr<LCCollectionVec> thirdFrameColl ( new LCCollectionVec (LCIO::TRACKERRAWDATA) );
	auto_ptr<LCCollectionVec> cdsFrameColl   ( new LCCollectionVec (LCIO::TRACKERRAWDATA) );
	auto_ptr<LCCollectionVec> zsFrameColl     ( new LCCollectionVec (LCIO::TRACKERDATA)    );
	CellIDEncoder< TrackerRawDataImpl > idEncoder1   (EUTELESCOPE::MATRIXDEFAULTENCODING, firstFrameColl.get());
	CellIDEncoder< TrackerRawDataImpl > idEncoder2   (EUTELESCOPE::MATRIXDEFAULTENCODING, secondFrameColl.get());
	CellIDEncoder< TrackerRawDataImpl > idEncoder3   (EUTELESCOPE::MATRIXDEFAULTENCODING, thirdFrameColl.get());
	CellIDEncoder< TrackerRawDataImpl > idEncoderCDS (EUTELESCOPE::MATRIXDEFAULTENCODING, cdsFrameColl.get());
	CellIDEncoder< TrackerDataImpl    > idEncoderZS  (EUTELESCOPE::ZSDATADEFAULTENCODING, zsFrameColl.get());

	// set here static parameters, while keeping the sensor id
	// setting for within the sensor loop

	if ( ( eudrbGlobalMode == "RAW3" ) || 
	     ( eudrbGlobalMode == "Mixed" ) ) {
	  idEncoder1["xMin"]             = 0;
	  idEncoder1["xMax"]             = _xMax - 1 - _markerPositionVec.size();
	  idEncoder1["yMin"]             = 0;
	  idEncoder1["yMax"]             = _yMax - 1;
	  idEncoder2["xMin"]             = 0;
	  idEncoder2["xMax"]             = _xMax - 1 - _markerPositionVec.size();
	  idEncoder2["yMin"]             = 0;
	  idEncoder2["yMax"]             = _yMax - 1;
	  idEncoder3["xMin"]             = 0;
	  idEncoder3["xMax"]             = _xMax - 1 - _markerPositionVec.size();
	  idEncoder3["yMin"]             = 0;
	  idEncoder3["yMax"]             = _yMax - 1;
	  idEncoderCDS["xMin"]           = 0;
	  idEncoderCDS["xMax"]           = _xMax - 1 - _markerPositionVec.size();
	  idEncoderCDS["yMin"]           = 0;
	  idEncoderCDS["yMax"]           = _yMax - 1;
	  idEncoderZS["sparsePixelType"] = _pixelType;
	} else if ( eudrbGlobalMode == "ZS" ) {
	  idEncoderZS["sparsePixelType"] = _pixelType;
	}

	if ( eventCounter % 10 == 0 ) 
	  streamlog_out ( MESSAGE4 ) << "Processing event " 
				     << setw(6) << setiosflags(ios::right) << ev->GetEventNumber() << " in run "
				     << setw(6) << setiosflags(ios::right) << setfill('0')  << ev->GetRunNumber() << setfill(' ')
				     << " (Total = " << setw(10) << eventCounter << ")" << resetiosflags(ios::left) << endl;
	
	// The detector event contains one sub-event for each
	// producer. It is very likely to have a sub event for the TLU
	// and another one for the EUDRB
	
	for (unsigned int iProducer = 0; iProducer < dev->NumEvents(); iProducer++ ) {
	  // get from the producer the event into a virtual base class
	  // that is to say "Event"
	  Event * subev = dev->GetEvent(iProducer);
	  streamlog_out ( DEBUG4 ) << "Processing producer number  " << iProducer << endl;
	  
	  // now try to see if the subev we got is a EUDRB event and
	  // continue only in this case
	  EUDRBEvent * eudev = dynamic_cast<EUDRBEvent*> (subev) ;
	  
	  if ( eudev ) {
	    // ok great this is a EUDRB event, now we need to loop on
	    // the number of boards the EUDRB producer is reading out
	    for (unsigned  int iDetector = 0; iDetector < eudev->NumBoards(); iDetector++) {
	      // EUDRBBoard is the wrapper class containing the real
	      // data we are interested in
	      EUDRBBoard & brd = eudev->GetBoard(iDetector);
	           
	      if (  ( eudrbGlobalMode == "RAW3" ) ||
		    ( ( eudrbGlobalMode == "Mixed" ) && ( eudrbSubMode[iDetector] == "RAW3" ) ) ) {
		
		// get arrays may throw a eudaq::Exception in case, for
		// instance, the array sizes are different. In this case
		// we should catch the exception, inform the user and then
		// skip the event
		EUDRBDecoder::arrays_t<short, short> array = eudrbDecoder->GetArrays<short,short>(brd);
		
		// prepare a TrackerRawDataImpl for each of the frame and
		// the corresponding cellIDEncoder.
		auto_ptr<TrackerRawDataImpl > firstFrame( new TrackerRawDataImpl );
		idEncoder1["sensorID"] = iDetector;
		idEncoder1.setCellID( firstFrame.get() );
		
		auto_ptr<TrackerRawDataImpl > secondFrame( new TrackerRawDataImpl );
		idEncoder2["sensorID"] = iDetector;
		idEncoder2.setCellID( secondFrame.get() );
		
		auto_ptr<TrackerRawDataImpl > thirdFrame( new TrackerRawDataImpl );
		idEncoder3["sensorID"] = iDetector;
		idEncoder3.setCellID( thirdFrame.get() );
		
		if ( _removeMarkerSwitch ) {
		  // the idea behind the marker removal procedure is that:
		  // markers are occurring always at the same column
		  // position, so we can repeat the procedure into a loop
		  // over all the rows. The marker positions are given by
		  // the user in the steering file and stored into the
		  // _markerPositionVec. For each row the part of the
		  // original vector in between two markers is copied into
		  // the stripped matrix.
		  
		  vector<short > firstStrippedVec(  ( _xMax - _markerPositionVec.size() ) * _yMax );
		  vector<short > secondStrippedVec( ( _xMax - _markerPositionVec.size() ) * _yMax );
		  vector<short > thirdStrippedVec(  ( _xMax - _markerPositionVec.size() ) * _yMax );
		  vector<short >::iterator currentFirstPos  = firstStrippedVec.begin();
		  vector<short >::iterator currentSecondPos = secondStrippedVec.begin();
		  vector<short >::iterator currentThirdPos  = thirdStrippedVec.begin();
		  vector<short >::iterator firstBegin       = array.m_adc[0].begin();
		  vector<short >::iterator secondBegin      = array.m_adc[1].begin();
		  vector<short >::iterator thirdBegin       = array.m_adc[2].begin();
		  
		  for ( int y = 0; y < _yMax ; y++ ) {
		    int offset = y * _xMax;
		    vector<int >::iterator marker = _markerPositionVec.begin();
		    // first of all copy the part of the array going from
		    // the row beginning to the first marker position
		    currentFirstPos    = copy( firstBegin + offset, 
					       firstBegin + ( *(marker) + offset ),
					       currentFirstPos );
		    currentSecondPos   = copy( secondBegin + offset,
					       secondBegin + ( *(marker) + offset ),
					       currentSecondPos );
		    currentThirdPos    = copy( thirdBegin + offset, 
					       thirdBegin + ( *(marker) + offset ),
					       currentThirdPos );
		    
		    while ( marker != _markerPositionVec.end() ) {
		      if ( marker < _markerPositionVec.end() - 1 ) {
			currentFirstPos   = copy( firstBegin + ( *(marker) + 1 + offset),
						  firstBegin + ( *(marker + 1 ) + offset),
						  currentFirstPos );
			currentSecondPos  = copy( secondBegin + ( *(marker) + 1 + offset),
						  secondBegin + ( *(marker + 1 ) + offset),
						  currentSecondPos );
			currentThirdPos   = copy( thirdBegin + ( *(marker) + 1 + offset),
						  thirdBegin + ( *(marker + 1 ) + offset),
						  currentThirdPos );
		      } else {
			// this is the case where we have to copy the part
			// of the original vector going from the last
			// marker and the end of the row.
			currentFirstPos = copy( firstBegin + ( *(marker) + 1 + offset ),
						firstBegin + offset + _xMax, 
						currentFirstPos );
			
			currentSecondPos = copy( secondBegin + ( *(marker) + 1 + offset ),
						 secondBegin + offset + _xMax, 
						 currentSecondPos );
			
			currentThirdPos = copy( thirdBegin + ( *(marker) + 1 + offset ),
						thirdBegin + offset + _xMax, 
						currentThirdPos );
		      }
		      ++marker;
		    }
		  }
		
		  firstFrame->setADCValues( firstStrippedVec );
		  secondFrame->setADCValues( secondStrippedVec );
		  thirdFrame->setADCValues( thirdStrippedVec );
		  
		
		} else {
		
		  // just move the adc values from the EUDRBDecoder::arrays_t to
		  // the trackerRawDataImpl
		  
		
		  firstFrame->setADCValues(  array.m_adc[0] );
		  secondFrame->setADCValues( array.m_adc[1] );
		  thirdFrame->setADCValues(  array.m_adc[2] );
		  
		}
		
		firstFrame->setTime(brd.PivotPixel());
		secondFrame->setTime(brd.PivotPixel());	      
		thirdFrame->setTime(brd.PivotPixel());
	      
		if ( _cdsCalculation ) {
		  auto_ptr<TrackerRawDataImpl> cdsFrame( new TrackerRawDataImpl );
		  idEncoderCDS["sensorID"] = iDetector;
		  idEncoderCDS.setCellID( cdsFrame.get() );
		  vector<short > cdsVector;
		  vector<short > firstFrameVec  = array.m_adc[0];
		  vector<short > secondFrameVec = array.m_adc[1];
		  vector<short > thirdFrameVec  = array.m_adc[2]; 
		  
		  // bad luck! the pivot pixel is obtained counting also
		  // the markers, so I prefer to calculate the cds as the
		  // marker should stay into and in case remove them afterward.
		  for (unsigned int iPixel = 0; iPixel < eudrbDecoder->NumPixels(brd); iPixel++) {
		    cdsVector.push_back( static_cast<short> (_polarity )* 
					 ( ( -1 * array.m_pivot[iPixel]    ) * firstFrameVec[iPixel] +
					   ( 2 * array.m_pivot[iPixel] - 1 ) * secondFrameVec[iPixel] +
					   ( 1 - array.m_pivot[iPixel]     ) * thirdFrameVec[iPixel] ) );
		  }	      
		  
		  
		  if ( _removeMarkerSwitch ) {
		    // strip away the markers...
		    vector<short > cdsStrippedVec( ( _xMax - _markerPositionVec.size() ) * _yMax );
		    vector<short >::iterator currentCDSPos = cdsStrippedVec.begin();
		    vector<short >::iterator cdsBegin      = cdsVector.begin();
		    for ( int y = 0; y < _yMax ; y ++ ) {
		      int offset = y * _xMax ;
		      vector<int >::iterator marker = _markerPositionVec.begin();
		      currentCDSPos = copy( cdsBegin + offset, 
					    cdsBegin + ( *(marker) + offset ),
					    currentCDSPos);
		      while ( marker != _markerPositionVec.end() ) {
			if ( marker < _markerPositionVec.end() - 1 ) {
			  currentCDSPos = copy( cdsBegin + ( *(marker) + 1 + offset),
						cdsBegin + ( *(marker + 1 ) + offset),
						currentCDSPos );
			} else {
			  currentCDSPos = copy( cdsBegin + ( *(marker) + 1 + offset ),
						cdsBegin + offset + _xMax, 
						currentCDSPos );
			}
			++marker;
		      }
		    }
		    cdsFrame->setADCValues( cdsStrippedVec );
		  } else {
		    cdsFrame->setADCValues( cdsVector );
		  }
		  cdsFrameColl->push_back( cdsFrame.release() );
		}	   
	      
	      
		// add the trackerRawData to the corresponding collection
		firstFrameColl->push_back(  firstFrame.release()   );
		secondFrameColl->push_back( secondFrame.release()  );
		thirdFrameColl->push_back(  thirdFrame.release()   );
	      } else if ( ( ( eudrbGlobalMode == "Mixed" ) && ( eudrbSubMode[iDetector] == "ZS" ) ) ||
			  ( eudrbGlobalMode == "ZS" ) ) {

		auto_ptr<TrackerDataImpl> zsFrame( new TrackerDataImpl );
		idEncoderZS["sensorID"] = iDetector;
		idEncoderZS.setCellID( zsFrame.get() ) ;

		// get the number of sparsified pixel in the
		// event. This is written in the board decoder
		unsigned int nPixel = eudrbDecoder->NumPixels(brd);
		streamlog_out ( DEBUG0 ) << "==================================================" << endl
					 << "Board " << iDetector << " working " << eudrbSubMode[iDetector] << " " 
					 << "contains " << nPixel << " sparse pixels." << endl;

		// for each different sparse pixel type implement a
		// case here below.
		if ( _pixelType == kEUTelSimpleSparsePixel ) {
		  auto_ptr<EUTelSparseDataImpl<EUTelSimpleSparsePixel > > 
		    sparseFrame( new EUTelSparseDataImpl<EUTelSimpleSparsePixel > ( zsFrame.get() ) );
		  
		  // now we can get the arrays containing the
		  // data. This is done done via the template method
		  // GetArrays and should be done inside the case
		  // implementation because the template parameters
		  // can be different from one implementation to
		  // another. 
		  
		  EUDRBDecoder::arrays_t<short, short> array = eudrbDecoder->GetArrays<short, short> ( brd ) ;
		  
		  // prepare a sparse pixel to be added to the sparse
		  // data
		  auto_ptr<EUTelSimpleSparsePixel> sparsePixel( new EUTelSimpleSparsePixel );
		  for (unsigned int iPixel = 0; iPixel < nPixel; iPixel++ ) {

		    if ( _removeMarkerSwitch ) {
		      // when removing the markers only the x
		      // coordinate is affected.
		      // First of all check if the current pixel is
		      // not on a marker column
		      short originalX = array.m_x[iPixel];
		      if ( find( _markerPositionVec.begin(), _markerPositionVec.end(), originalX ) == _markerPositionVec.end() ) {
			short diff      = (short) count_if ( _markerPositionVec.begin(), _markerPositionVec.end(), 
							     bind2nd(less<short> (), originalX ) );
			sparsePixel->setXCoord( originalX - diff ) ;
			sparsePixel->setYCoord( array.m_y[iPixel]   );
			sparsePixel->setSignal( array.m_adc[0][iPixel] );
			streamlog_out ( DEBUG0 ) << (* (sparsePixel.get() ) ) << endl;
			sparseFrame->addSparsePixel( sparsePixel.get() ) ;
		      } else {
			streamlog_out ( WARNING2 ) << "Found a sparse pixel on a marker column. Not adding it to the frame" << endl;
		      }

		    } else {
		      sparsePixel->setXCoord( array.m_x[iPixel]   );
		      sparsePixel->setYCoord( array.m_y[iPixel]   );
		      sparsePixel->setSignal( array.m_adc[0][iPixel] );
		      streamlog_out ( DEBUG0 ) << (* (sparsePixel.get() ) ) << endl;
		      sparseFrame->addSparsePixel( sparsePixel.get() ) ;
		    }

		  }
		  zsFrameColl->push_back( zsFrame.release() ) ;
		}
	      }
	    }
	  } else {
	    streamlog_out (DEBUG3) << "Not a EUDRBEvent, very likely a TLUEvent " << endl;
	  }
	}
	// the collection pointers have be to released when added to
	// the event because the ownership has to be passed to the
	// event.
	if ( ( eudrbGlobalMode == "RAW3" ) || ( eudrbGlobalMode == "Mixed" ) ) {
	  event->addCollection(firstFrameColl.release(),  _firstFrameCollectionName);
	  event->addCollection(secondFrameColl.release(), _secondFrameCollectionName);
	  event->addCollection(thirdFrameColl.release(),  _thirdFrameCollectionName);
	  if ( _cdsCalculation ) event->addCollection( cdsFrameColl.release(), _cdsCollectionName );
	}
	if ( ( eudrbGlobalMode == "ZS" ) || ( eudrbGlobalMode == "Mixed" ) ) {
	  event->addCollection(zsFrameColl.release(), _zsFrameCollectionName);
	}

	// the event has to be released has well, but remember to
	// delete it afterwards.
	EUTelEventImpl * dummyEvt = event.release();
	ProcessorMgr::instance()->processEvent( static_cast<LCEventImpl*> (dummyEvt) );
	delete dummyEvt;
      }  catch (eudaq::Exception& e) {
	message<ERROR> ( log() << e.what() << endl
			 << "Skipping the current event" ) ;
      }

    }
    
  }
}    




void EUTelMimoTelReader::end () {
  message<MESSAGE> ("Successfully finished") ;
}

#endif

