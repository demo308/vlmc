/*****************************************************************************
 * ClipWorkflow.cpp : Clip workflow. Will extract a single frame from a VLCMedia
 *****************************************************************************
 * Copyright (C) 2008-2010 VideoLAN
 *
 * Authors: Hugo Beauzee-Luyssen <hugo@vlmc.org>
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

#include "vlmc.h"
#include "Clip.h"
#include "ClipWorkflow.h"
#include "LightVideoFrame.h"
#include "Media.h"
#include "MemoryPool.hpp"
#include "WaitCondition.hpp"
#include "VLCMedia.h"
#include "VLCMediaPlayer.h"

#include <QReadWriteLock>
#include <QWaitCondition>
#include <QtDebug>

ClipWorkflow::ClipWorkflow( Clip::Clip* clip ) :
                m_mediaPlayer(NULL),
                m_clip( clip ),
                m_state( ClipWorkflow::Stopped )
{
    m_stateLock = new QReadWriteLock;
    m_initWaitCond = new WaitCondition;
    m_pausingStateWaitCond = new WaitCondition;
    m_renderLock = new QMutex;
    m_availableBuffersMutex = new QMutex;
    m_computedBuffersMutex = new QMutex;
    m_renderWaitCond = new WaitCondition;
}

ClipWorkflow::~ClipWorkflow()
{
    delete m_renderWaitCond;
    delete m_computedBuffersMutex;
    delete m_availableBuffersMutex;
    delete m_renderLock;
    delete m_pausingStateWaitCond;
    delete m_initWaitCond;
    delete m_stateLock;
}

void    ClipWorkflow::initialize()
{
    setState( ClipWorkflow::Initializing );

    m_vlcMedia = new LibVLCpp::Media( m_clip->getMedia()->mrl() );
    m_currentPts = -1;
    m_previousPts = -1;
    m_pauseDuration = -1;
    initVlcOutput();
    m_mediaPlayer = MemoryPool<LibVLCpp::MediaPlayer>::getInstance()->get();
    m_mediaPlayer->setMedia( m_vlcMedia );

    connect( m_mediaPlayer, SIGNAL( playing() ), this, SLOT( loadingComplete() ), Qt::DirectConnection );
    connect( m_mediaPlayer, SIGNAL( endReached() ), this, SLOT( clipEndReached() ), Qt::DirectConnection );
    m_mediaPlayer->play();
}

void    ClipWorkflow::loadingComplete()
{
    adjustBegin();
    disconnect( m_mediaPlayer, SIGNAL( playing() ), this, SLOT( loadingComplete() ) );
    connect( m_mediaPlayer, SIGNAL( playing() ), this, SLOT( mediaPlayerUnpaused() ), Qt::DirectConnection );
    connect( m_mediaPlayer, SIGNAL( paused() ), this, SLOT( mediaPlayerPaused() ), Qt::DirectConnection );
    QMutexLocker    lock( m_initWaitCond->getMutex() );
    setState( Rendering );
    m_initWaitCond->wake();
}

void    ClipWorkflow::adjustBegin()
{
    if ( m_clip->getMedia()->fileType() == Media::Video ||
         m_clip->getMedia()->fileType() == Media::Audio )
    {
        m_mediaPlayer->setTime( m_clip->begin() /
                                m_clip->getMedia()->fps() * 1000 );
    }
}

bool    ClipWorkflow::isEndReached() const
{
    QReadLocker lock( m_stateLock );
    return m_state == ClipWorkflow::EndReached;
}

bool    ClipWorkflow::isStopped() const
{
    QReadLocker lock( m_stateLock );
    return m_state == ClipWorkflow::Stopped;
}

ClipWorkflow::State     ClipWorkflow::getState() const
{
    return m_state;
}

void    ClipWorkflow::clipEndReached()
{
    setState( EndReached );
}

Clip*     ClipWorkflow::getClip()
{
    return m_clip;
}

void            ClipWorkflow::stop()
{
    if ( m_mediaPlayer )
    {
        m_mediaPlayer->stop();
        disconnect( m_mediaPlayer, SIGNAL( endReached() ), this, SLOT( clipEndReached() ) );
        MemoryPool<LibVLCpp::MediaPlayer>::getInstance()->release( m_mediaPlayer );
        m_mediaPlayer = NULL;
        setState( Stopped );
        delete m_vlcMedia;
        flushComputedBuffers();
        releasePrealocated();
    }
}

void
ClipWorkflow::setTime( qint64 time )
{
    m_mediaPlayer->setTime( time );
    resyncClipWorkflow();
    QWriteLocker    lock( m_stateLock );
    if ( m_state == ClipWorkflow::Paused )
    {
        m_mediaPlayer->pause();
        m_state = ClipWorkflow::UnpauseRequired;
    }
}

bool            ClipWorkflow::isRendering() const
{
    QReadLocker lock( m_stateLock );
    return m_state == ClipWorkflow::Rendering;
}

void            ClipWorkflow::setState( State state )
{
    QWriteLocker    lock( m_stateLock );
    m_state = state;
}

QReadWriteLock* ClipWorkflow::getStateLock()
{
    return m_stateLock;
}

void        ClipWorkflow::waitForCompleteInit()
{
    if ( isRendering() == false )
    {
        QMutexLocker    lock( m_initWaitCond->getMutex() );
        m_initWaitCond->waitLocked();
    }
}

LibVLCpp::MediaPlayer*       ClipWorkflow::getMediaPlayer()
{
    return m_mediaPlayer;
}

bool        ClipWorkflow::preGetOutput()
{
    //Computed buffer mutex is already locked by underlying clipworkflow getoutput method
    if ( getNbComputedBuffers() == 0 )
        return false;
    return true;
}

void        ClipWorkflow::postGetOutput()
{
    //If we're running out of computed buffers, refill our stack.
    if ( getNbComputedBuffers() < getMaxComputedBuffers() / 3 )
    {
        QWriteLocker        lock( m_stateLock );
        if ( m_state == ClipWorkflow::Paused )
        {
            m_state = ClipWorkflow::UnpauseRequired;
//            This will act like an "unpause";
            m_mediaPlayer->pause();
        }
    }
}

void        ClipWorkflow::commonUnlock()
{
    //Don't test using availableBuffer, as it may evolve if a buffer is required while
    //no one is available : we would spawn a new buffer, thus modifying the number of available buffers
    if ( getNbComputedBuffers() >= getMaxComputedBuffers() )
    {
        setState( ClipWorkflow::PauseRequired );
        m_mediaPlayer->pause();
    }
}

void    ClipWorkflow::computePtsDiff( qint64 pts )
{
    if ( m_previousPts == -1 )
        m_previousPts = pts;
    if ( m_currentPts == -1 )
        m_currentPts = pts;
    if ( m_pauseDuration != -1 )
    {
        m_previousPts = m_currentPts + m_pauseDuration;
        m_pauseDuration = -1;
    }
    else
        m_previousPts = m_currentPts;
    m_currentPts = qMax( pts, m_previousPts );
}

void    ClipWorkflow::mediaPlayerPaused()
{
    setState( ClipWorkflow::Paused );
    m_beginPausePts = mdate();
}

void    ClipWorkflow::mediaPlayerUnpaused()
{
    setState( ClipWorkflow::Rendering );
    m_pauseDuration = mdate() - m_beginPausePts;
}

void    ClipWorkflow::resyncClipWorkflow()
{
    flushComputedBuffers();
    m_previousPts = -1;
    m_currentPts = -1;
}

void
ClipWorkflow::setFullSpeedRender( bool val )
{
    m_fullSpeedRender = val;
}

void
ClipWorkflow::mute()
{
    stop();
    setState( Muted );
}

void
ClipWorkflow::unmute()
{
    setState( Stopped );
}

void
ClipWorkflow::requireResync()
{
    m_resyncRequired = 1;
}

bool
ClipWorkflow::isResyncRequired()
{
    if ( m_resyncRequired == 1 )
    {
        m_resyncRequired = 0;
        return true;
    }
    return false;
}
