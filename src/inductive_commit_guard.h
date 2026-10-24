/*
 * This program source code file is part of KiCad, a free EDA CAD application.
 *
 * Copyright The KiCad Developers, see AUTHORS.TXT for contributors.
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
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#ifndef INDUCTIVE_COMMIT_GUARD_H
#define INDUCTIVE_COMMIT_GUARD_H

#include <iterator>
#include <vector>

#include <undo_redo_container.h>
#include <wx/string.h>

/**
 * Make an abandoned commit revert instead of silently sticking.
 *
 * KiCad's COMMIT destructor does **not** roll back.  `common/commit.cpp` frees
 * the saved undo images and nothing else; `BOARD_COMMIT` and `SCH_COMMIT` both
 * have empty destructors.  So a commit that is built and never pushed leaves the
 * document mutated with **no undo entry at all**.
 *
 * That turns an ordinary error path into silent corruption.  A tool calls
 * `commit.Modify( item )`, changes it, and then throws -- an argument it only
 * validates late, a lookup that fails on the third item of a list, a KiCad call
 * that raises.  The panel catches the exception and reports a routine failed
 * tool call; the model reads "that did not work" and moves on; and the board is
 * left changed in a way the user cannot undo, because Ctrl+Z now targets
 * whatever they did before the agent ran.  Nothing anywhere reports it.
 *
 * A crash would be better than this: a crash is noticed.
 *
 * So every commit in Inductive's tools is wrapped.  Push normally and call
 * Pushed(); on any other exit the destructor reverts.  The pattern is:
 *
 *     BOARD_COMMIT commit( aFrame );
 *     INDUCTIVE_COMMIT_GUARD guard( commit );
 *
 *     commit.Modify( fp );        // before the mutation, so undo has the old state
 *     fp->SetPosition( to );
 *
 *     commit.Push( _( "Move Footprint" ) );
 *     guard.Pushed();             // from here the guard does nothing
 *
 * The right long-term fix is to validate everything before opening a commit at
 * all, and the tools do that wherever they can.  This exists because "wherever
 * they can" is not "always", and because the next tool someone adds will not
 * have read this comment.
 */
template <typename COMMIT_TYPE>
class INDUCTIVE_COMMIT_GUARD
{
public:
    explicit INDUCTIVE_COMMIT_GUARD( COMMIT_TYPE& aCommit ) :
            m_commit( aCommit ),
            m_pushed( false )
    {}

    INDUCTIVE_COMMIT_GUARD( const INDUCTIVE_COMMIT_GUARD& ) = delete;
    INDUCTIVE_COMMIT_GUARD& operator=( const INDUCTIVE_COMMIT_GUARD& ) = delete;

    ~INDUCTIVE_COMMIT_GUARD()
    {
        if( m_pushed )
            return;

        // A destructor that throws during stack unwinding terminates the
        // process, and this one runs precisely when an exception is already in
        // flight.  Revert() reaches into the view and the model, so it is not
        // obviously incapable of raising; swallowing is the only correct
        // behaviour here.  The failure it is covering has already been reported
        // to the user through the tool result.
        try
        {
            m_commit.Revert();
        }
        catch( ... )
        {
        }
    }

    /// Called immediately after a successful Push(), to disarm the guard.
    void Pushed() { m_pushed = true; }

private:
    COMMIT_TYPE& m_commit;
    bool         m_pushed;
};


/**
 * One chat request, one undo step.
 *
 * A request runs many tools and each pushes its own undo step -- placement,
 * routing, every zone refill -- so taking back one request took a dozen Cmd+Z.
 * Begin() marks the top of the undo list when the request starts; End() folds
 * every step pushed above the mark into the oldest of them.
 *
 * The steps keep their order. KiCad undoes a step's changes newest first and
 * reverses the list for redo, exactly as for the steps it appends itself
 * (APPEND_UNDO), so an item changed by several tools ends where it started.
 * The mark is the top step itself, not a count: KiCad drops the oldest steps
 * once the history is full, which a count would mistake for nothing added. A
 * dropped mark's address can come back as a new step, so the mark is also known
 * by its first change and its description (a tool appending to it adds changes
 * at the end). With the mark gone, every step left is newer: all are folded.
 */
class INDUCTIVE_UNDO_GROUP
{
public:
    template <typename FRAME>
    void Begin( FRAME* aFrame )
    {
        m_mark = aFrame->PopCommandFromUndoList();

        if( m_mark )
        {
            m_markItem = m_mark->GetCount() ? m_mark->GetPickedItem( 0 ) : nullptr;
            m_markLink = m_mark->GetCount() ? m_mark->GetPickedItemLink( 0 ) : nullptr;
            m_markName = m_mark->GetDescription();
            aFrame->PushCommandToUndoList( m_mark );   // same depth: nothing is trimmed
        }

        m_open = true;
    }

    template <typename FRAME>
    void End( FRAME* aFrame, const wxString& aName )
    {
        if( !m_open )
            return;

        m_open = false;

        std::vector<PICKED_ITEMS_LIST*> steps;   // newest first

        for( int n = aFrame->GetUndoCommandCount(); n > 0; --n )
        {
            PICKED_ITEMS_LIST* step = aFrame->PopCommandFromUndoList();

            if( !step )
                break;

            if( isMark( step ) )
            {
                aFrame->PushCommandToUndoList( step );
                break;
            }

            steps.push_back( step );
        }

        if( steps.empty() )
            return;

        PICKED_ITEMS_LIST* merged = steps.back();   // the oldest

        for( auto it = std::next( steps.rbegin() ); it != steps.rend(); ++it )
        {
            for( unsigned ii = 0; ii < ( *it )->GetCount(); ++ii )
                merged->PushItem( ( *it )->GetItemWrapper( ii ) );

            ( *it )->ClearItemsList();   // the pickers now belong to merged
            delete *it;
        }

        if( steps.size() > 1 )
            merged->SetDescription( aName );

        aFrame->PushCommandToUndoList( merged );
    }

private:
    bool isMark( PICKED_ITEMS_LIST* aStep ) const
    {
        return aStep == m_mark && aStep->GetDescription() == m_markName
               && ( aStep->GetCount() ? aStep->GetPickedItem( 0 ) : nullptr ) == m_markItem
               && ( aStep->GetCount() ? aStep->GetPickedItemLink( 0 ) : nullptr ) == m_markLink;
    }

    PICKED_ITEMS_LIST* m_mark = nullptr;
    EDA_ITEM*          m_markItem = nullptr;
    EDA_ITEM*          m_markLink = nullptr;
    wxString           m_markName;
    bool               m_open = false;
};

#endif  // INDUCTIVE_COMMIT_GUARD_H
