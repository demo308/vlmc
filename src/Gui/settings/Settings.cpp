/*****************************************************************************
 * Settings.cpp: generic preferences interface
 *****************************************************************************
 * Copyright (C) 2008-2010 VideoLAN
 *
 * Authors: Clement CHAVANCE <kinder@vlmc.org>
 *          Ludovic Fauvet <etix@l0cal.com>
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

#include "Settings.h"

#include "PreferenceWidget.h"
#include "SettingsManager.h"
#include "Panel.h"

#include <QApplication>
#include <QDialogButtonBox>
#include <QAbstractButton>
#include <QIcon>
#include <QLabel>
#include <QScrollArea>
#include <QHBoxLayout>
#include <QStackedLayout>

#include <QtDebug>

Settings::Settings( SettingsManager::Type type, QWidget* parent, Qt::WindowFlags f ) :
    QDialog( parent, f ),
    m_type( type )
{
    setMinimumHeight( 400 );
    setMinimumWidth( 600 );

    m_buttons = new QDialogButtonBox( Qt::Horizontal, this );
    m_buttons->setStandardButtons( QDialogButtonBox::Ok |
                                   QDialogButtonBox::Cancel |
                                   QDialogButtonBox::Apply );

    connect( m_buttons, SIGNAL( clicked( QAbstractButton* ) ),
             this, SLOT( buttonClicked( QAbstractButton* ) ) );

    // Create the layout
    buildLayout();

    connect( m_panel, SIGNAL( changePanel( int ) ),
             this, SLOT( switchWidget( int ) ) );
}

void        Settings::addCategory( const char* name,
                                   SettingsManager::Type type,
                                   const QIcon& icon,
                                   const QString& label )
{
    PreferenceWidget    *pWidget = new PreferenceWidget( name, type, this );

    m_stackedLayout->addWidget( pWidget );

    // Create a button linked to the widget using its index
    m_panel->addButton( label, icon, m_stackedLayout->count() - 1 );

    switchWidget( 0 );
}

void
Settings::buildLayout()
{
    // Create the left panel
    m_panel = new Panel( this );
    m_panel->setMaximumWidth( 130 );

    // Create the master layout
    QGridLayout* mLayout = new QGridLayout( this );
    mLayout->addWidget( m_panel, 0, 0, 2, 1 );

    m_title = new QLabel( this );
    m_stackedLayout = new QStackedLayout;

    // Set the font and text of the panel title
    QFont titleFont = QApplication::font();
    titleFont.setPointSize( titleFont.pointSize() + 6 );
    titleFont.setFamily( "Verdana" );
    m_title->setFont( titleFont );

    mLayout->addWidget( m_title, 0, 1, 1, 2 );
    mLayout->addLayout( m_stackedLayout, 1, 2, 1, 2 );
    mLayout->addWidget( m_buttons, 2, 2 );
}

void    Settings::buttonClicked( QAbstractButton* button )
{
    bool  save = false;
    bool  hide = false;

    switch ( m_buttons->standardButton( button ) )
    {
        case QDialogButtonBox::Ok :
            save = true;
            hide = true;
            break;
        case QDialogButtonBox::Cancel :
            hide = true;
            break;
        case QDialogButtonBox::Apply :
            save = true;
            break;
        default :
            break;
    }
    if ( save == true )
    {
        // Ask each widget to save their state
        for ( int i = 0; i < m_stackedLayout->count(); ++i )
            qobject_cast<PreferenceWidget*>( m_stackedLayout->widget( i ) )->save();
        //If we're handling vlmc preferences, save the value in the QSettings
        if ( m_type == SettingsManager::Vlmc )
            SettingsManager::getInstance()->save();
    }
    if ( hide == true )
        setVisible( false );
}

void    Settings::switchWidget( int index )
{
    m_stackedLayout->setCurrentIndex( index );

    // Reload the translated title
    retranslateUi();
}

void    Settings::changeEvent( QEvent *e )
{
    switch ( e->type() )
    {
    case QEvent::LanguageChange:
        retranslateUi();
        break;
    default:
        break;
    }
}

void    Settings::retranslateUi()
{
    PreferenceWidget* pWidget =
            qobject_cast<PreferenceWidget*>( m_stackedLayout->widget( m_stackedLayout->currentIndex() ) );
    Q_ASSERT( pWidget != NULL );

    // Translate the category name using the current locale
    QString text = tr( pWidget->category() );

    if ( text.length() >= 1 )
        text[0] = text[0].toUpper();
    m_title->setText( text );
}
