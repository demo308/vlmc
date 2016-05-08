/*****************************************************************************
 * ClipWorkflow.cpp : Clip workflow. Will extract frames from a media
 *****************************************************************************
 * Copyright (C) 2008-2016 VideoLAN
 *
 * Authors: Hugo Beauzée-Luyssen <hugo@beauzee.fr>
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

#include <QMutex>
#include <QReadWriteLock>
#include <QStringBuilder>
#include <QWaitCondition>

#include "vlmc.h"
#include "Media/Clip.h"
#include "ClipWorkflow.h"
#include "Backend/ISource.h"
#include "Backend/ISourceRenderer.h"
#include "Renderer/ClipSmemRenderer.h"
#include "Media/Media.h"
#include "Tools/RendererEventWatcher.h"
#include "Workflow/Types.h"

#include "Tools/VlmcDebug.h"

ClipWorkflow::ClipWorkflow( Clip* clip )
    : m_renderer( nullptr )
    , m_eventWatcher( nullptr )
    , m_clip( clip )
    , m_state( ClipWorkflow::Stopped )
    , m_fullSpeedRender( false )
    , m_muted( false )
{
    m_stateLock = new QReadWriteLock;
    m_initWaitCond = new QWaitCondition;
    m_renderLock = new QMutex;
    m_renderWaitCond = new QWaitCondition;
    m_eventWatcher = new RendererEventWatcher;
}

ClipWorkflow::~ClipWorkflow()
{
    stop();
    delete m_eventWatcher;
    delete m_renderWaitCond;
    delete m_renderLock;
    delete m_initWaitCond;
    delete m_stateLock;
}

Workflow::Frame*
ClipWorkflow::getOutput( Workflow::TrackType trackType, ClipSmemRenderer::GetMode mode, qint64 currentFrame )
{
    if ( m_clip->media()->fileType() == Media::Image )
        mode = ClipSmemRenderer::Get;

    auto ret = m_renderer->getOutput( trackType, mode, currentFrame );
    if ( ret )
    {
        computePtsDiff( ret->pts(), trackType );
        ret->ptsDiff = m_currentPts[trackType] - m_previousPts[trackType];
    }
    if ( trackType == Workflow::VideoTrack )
    {
        auto newFrame = applyFilters( ret, currentFrame );
        if ( newFrame != nullptr )
            ret->setBuffer( newFrame );
    }

    return ret;
}

void
ClipWorkflow::initialize( quint32 width, quint32 height )
{
    QWriteLocker lock( m_stateLock );
    m_state = ClipWorkflow::Initializing;

    delete m_renderer;
    m_renderer = new ClipSmemRenderer( m_clip, width, height, m_fullSpeedRender );
    if ( m_clip->formats() & Clip::Video )
        initFilters();

    for ( int i = 0; i < Workflow::NbTrackType; ++i )
    {
        m_currentPts[i] = -1;
        m_previousPts[i] = -1;
    }
    m_pauseDuration = -1;

    //Use QueuedConnection to avoid getting called from intf-event callback, as
    //we will trigger intf-event callback as well when setting time for this clip,
    //thus resulting in a deadlock.
    connect( m_renderer->eventWatcher(), SIGNAL( playing() ), this, SLOT( loadingComplete() ), Qt::QueuedConnection );
    connect( m_renderer->eventWatcher(), SIGNAL( endReached() ), this, SLOT( clipEndReached() ), Qt::DirectConnection );
    connect( m_renderer->eventWatcher(), SIGNAL( errorEncountered() ), this, SLOT( errorEncountered() ) );
    connect( m_renderer->eventWatcher(), &RendererEventWatcher::stopped, this, &ClipWorkflow::mediaPlayerStopped );
    m_renderer->start();
}

void
ClipWorkflow::loadingComplete()
{
    adjustBegin();
    disconnect( m_renderer->eventWatcher(), SIGNAL( playing() ), this, SLOT( loadingComplete() ) );
    connect( m_renderer->eventWatcher(), SIGNAL( playing() ), this, SLOT( mediaPlayerUnpaused() ), Qt::DirectConnection );
    connect( m_renderer->eventWatcher(), SIGNAL( paused() ), this, SLOT( mediaPlayerPaused() ), Qt::DirectConnection );
    QWriteLocker lock( m_stateLock );
    m_isRendering = true;
    m_state = Rendering;
    m_initWaitCond->wakeAll();
}

void
ClipWorkflow::adjustBegin()
{
    if ( m_clip->media()->fileType() == Media::Video ||
         m_clip->media()->fileType() == Media::Audio )
    {
        m_renderer->setTime( m_clip->begin() /
                                m_clip->media()->source()->fps() * 1000 );
    }
}

ClipWorkflow::State
ClipWorkflow::getState() const
{
    return m_state;
}

void
ClipWorkflow::clipEndReached()
{
    m_state = EndReached;
}

void
ClipWorkflow::stop()
{
    if ( m_renderer != nullptr )
        m_renderer->stop();
}

void
ClipWorkflow::setTime( qint64 time )
{
    vlmcDebug() << "Setting ClipWorkflow" << m_clip->uuid() << "time:" << time;
    m_renderer->setTime( time );
    resyncClipWorkflow();
    m_renderer->setPause( false );
}

bool
ClipWorkflow::waitForCompleteInit()
{
    QReadLocker lock( m_stateLock );

    if ( m_state != ClipWorkflow::Rendering && m_state != ClipWorkflow::Error )
    {
        m_initWaitCond->wait( m_stateLock );

        if ( m_state != ClipWorkflow::Rendering )
            return false;
    }
    return true;
}

void
ClipWorkflow::computePtsDiff( qint64 pts, Workflow::TrackType trackType )
{
    if ( m_pauseDuration != -1 )
    {
        //No need to check for m_currentPtr before, as we can't start in paused mode.
        //so m_currentPts will not be -1
        m_previousPts[trackType] = m_currentPts[trackType] + m_pauseDuration;
        m_pauseDuration = -1;
    }
    else
        m_previousPts[trackType] = m_currentPts[trackType];
    m_currentPts[trackType] = qMax( pts, m_previousPts[trackType] );
}

void
ClipWorkflow::flushComputedBuffers()
{
    m_renderer->flushComputedBuffers();
}

void
ClipWorkflow::mediaPlayerPaused()
{
    m_state = ClipWorkflow::Paused;
    m_beginPausePts = mdate();
}

void
ClipWorkflow::mediaPlayerUnpaused()
{
    m_state = ClipWorkflow::Rendering;
    m_pauseDuration = mdate() - m_beginPausePts;
}

void
ClipWorkflow::mediaPlayerStopped()
{
    m_eventWatcher->disconnect();
    if ( m_state != Error )
        m_state = Stopped;
    flushComputedBuffers();
    m_isRendering = false;

    m_initWaitCond->wakeAll();
    m_renderWaitCond->wakeAll();
}

void
ClipWorkflow::resyncClipWorkflow()
{
    flushComputedBuffers();
    for ( int i = 0; i < Workflow::NbTrackType; ++i )
    {
        m_currentPts[i] = -1;
        m_previousPts[i] = -1;
    }
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
    m_muted = true;
}

void
ClipWorkflow::unmute()
{
    m_muted = false;
}

bool
ClipWorkflow::isMuted() const
{
    return m_muted;
}

void
ClipWorkflow::requireResync()
{
    m_resyncRequired = true;
}

bool
ClipWorkflow::isResyncRequired()
{
    if ( m_resyncRequired == true )
    {
        m_resyncRequired = false;
        return true;
    }
    return false;
}

void
ClipWorkflow::errorEncountered()
{
    m_state = Error;
    emit error( this );
}

bool
ClipWorkflow::shouldRender() const
{
    ClipWorkflow::State state = m_state;
    return ( state != ClipWorkflow::Error &&
             state != ClipWorkflow::Stopped);
}

qint64
ClipWorkflow::length() const
{
    return m_clip->length();
}

EffectUser::Type
ClipWorkflow::effectType() const
{
    return EffectUser::ClipEffectUser;
}
