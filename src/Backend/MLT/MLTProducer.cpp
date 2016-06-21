/*****************************************************************************
 * MLTProducer.cpp:  Wrapper of Mlt::Producer
 *****************************************************************************
 * Copyright (C) 2008-2016 VideoLAN
 *
 * Authors: Yikei Lu <luyikei.qmltu@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#include "MLTProducer.h"
#include "MLTProfile.h"
#include "MLTBackend.h"

#include <mlt++/MltProducer.h>
#include <cstring>
#include <cassert>

using namespace Backend::MLT;

MLTProducer::MLTProducer()
    : m_producer( nullptr )
    , m_callback( nullptr )
    , m_paused( false )
    , m_nbVideoTracks( 0 )
    , m_nbAudioTracks( 0 )
{

}

void
MLTProducer::calcTracks()
{
    char s[70];
    int  nbStreams = m_producer->get_int( "meta.media.nb_streams" );
    for ( int i = 0; i < nbStreams; ++i )
    {
        sprintf( s, "meta.media.%d.stream.type", i );
        auto type = m_producer->get( s );

        if ( type == nullptr )
            continue;

        if ( strcmp( type, "video" ) == 0 )
            m_nbVideoTracks++;
        else if ( strcmp( type, "audio" ) == 0 )
            m_nbAudioTracks++;
    }
}

MLTProducer::MLTProducer( Mlt::Producer* producer, IProducerEventCb* callback )
    : MLTProducer()
{
    m_producer = producer;
    m_service  = producer;
    setCallback( callback );
    calcTracks();
}

MLTProducer::MLTProducer( IProfile& profile, const char* path, IProducerEventCb* callback )
    : MLTProducer()
{
    std::string temp = std::string( "avformat:" ) + path;
    MLTProfile& mltProfile = static_cast<MLTProfile&>( profile );
    m_producer = new Mlt::Producer( *mltProfile.m_profile, "loader", temp.c_str() );
    m_service  = m_producer;
    setCallback( callback );
    calcTracks();
}

MLTProducer::MLTProducer( const char* path, IProducerEventCb* callback )
    : MLTProducer( Backend::instance()->profile(), path, callback )
{
}


MLTProducer::~MLTProducer()
{
    delete m_producer;
}

void
MLTProducer::onPropertyChanged( void*, MLTProducer* self, const char* id )
{
    if ( self == nullptr || self->m_callback == nullptr )
        return;

    if ( strcmp( id, "_position" ) == 0 )
    {
        self->m_callback->onPositionChanged( self->position() );
        if ( self->position() >= self->playableLength() - 1 )
            self->m_callback->onEndReached();
    }
    else if ( strcmp( id, "length" ) == 0 )
        self->m_callback->onLengthChanged( self->playableLength() );

}

void
MLTProducer::setCallback( Backend::IProducerEventCb* callback )
{
    if ( callback == nullptr )
        return;

    m_callback = callback;
    m_producer->listen( "property-changed", this, (mlt_listener)MLTProducer::onPropertyChanged );
}

const char*
MLTProducer::path() const
{
    return m_producer->get( "resource" );
}

int64_t
MLTProducer::begin() const
{
    return m_producer->get_in();
}

int64_t
MLTProducer::end() const
{
    return m_producer->get_out();
}

void
MLTProducer::setBegin( int64_t begin )
{
    m_producer->set( "in", (int)begin );
}

void
MLTProducer::setEnd( int64_t end )
{
    m_producer->set( "out", (int)end );
}

void
MLTProducer::setBoundaries( int64_t begin, int64_t end )
{
    if ( end == EndOfParent )
        // parent() will be m_producer itself if it has no parent
        end = m_producer->parent().get_out();
    m_producer->set_in_and_out( begin, end );
}

Backend::IProducer*
MLTProducer::cut( int64_t begin, int64_t end )
{
    return new MLTProducer( m_producer->cut( begin, end ) );
}

bool
MLTProducer::isCut() const
{
    return m_producer->is_cut();
}

bool
MLTProducer::sameClip( Backend::IProducer& that ) const
{
    MLTProducer* producer = dynamic_cast<MLTProducer*>( &that );
    assert( producer );

    return m_producer->same_clip( *producer->m_producer );
}

bool
MLTProducer::runsInto( Backend::IProducer& that ) const
{
    MLTProducer* producer = dynamic_cast<MLTProducer*>( &that );
    assert( producer );

    return m_producer->runs_into( *producer->m_producer );
}

int64_t
MLTProducer::playableLength() const
{
    return m_producer->get_playtime();
}

int64_t
MLTProducer::length() const
{
    return m_producer->get_length();
}

const char*
MLTProducer::lengthTime() const
{
    return m_producer->get_length_time( mlt_time_clock );
}

int64_t
MLTProducer::position() const
{
    return m_producer->position();
}

void
MLTProducer::setPosition( int64_t position )
{
    m_producer->seek( position );
}

int64_t
MLTProducer::frame() const
{
    return m_producer->frame();
}

double
MLTProducer::fps() const
{
    return m_producer->get_fps();
}

int
MLTProducer::width() const
{
    return m_producer->get_int( "width" );
}

int
MLTProducer::height() const
{
    return m_producer->get_int( "height" );
}

bool
MLTProducer::hasVideo() const
{
    return m_nbVideoTracks > 0;
}

int
MLTProducer::nbVideoTracks() const
{
    return m_nbVideoTracks;
}

bool
MLTProducer::hasAudio() const
{
    return m_nbAudioTracks > 0;
}

int
MLTProducer::nbAudioTracks() const
{
    return m_nbAudioTracks;
}

void
MLTProducer::playPause()
{
    if ( m_paused )
        m_producer->set_speed( 1.0 );
    else
        m_producer->set_speed( 0.0 );
    m_paused = !m_paused;

    if ( m_callback )
    {
        if ( m_paused == true )
            m_callback->onPaused();
        else
            m_callback->onPlaying();
    }
}

bool
MLTProducer::isPaused() const
{
    return m_paused;
}

void
MLTProducer::setPause( bool isPaused )
{
    // It will be negated again in playPause.
    m_paused = !isPaused;
    playPause();
}

void
MLTProducer::nextFrame()
{
    if ( m_producer->position() < m_producer->get_out() )
    {
        if ( isPaused() == false )
            playPause();
        m_producer->seek( m_producer->position() + 1 );
    }
}

void
MLTProducer::previousFrame()
{
    if ( m_producer->get_in() < m_producer->position() )
    {
        if ( isPaused() == false )
            playPause();
        m_producer->seek( m_producer->position() - 1 );
    }
}

bool
MLTProducer::isBlank() const
{
    return m_producer->is_blank();
}
