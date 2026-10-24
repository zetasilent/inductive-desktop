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

#ifndef INDUCTIVE_BOARD_TOOLS_H
#define INDUCTIVE_BOARD_TOOLS_H

#include <nlohmann/json_fwd.hpp>
class BOARD;
class PCB_EDIT_FRAME;
class INDUCTIVE_PANEL;

/**
 * Give the Inductive sidebar the board editor's capabilities.
 *
 * Registers the document summary the model is shown, the slash commands the
 * board editor offers, and the tools the model may call against the open board.
 *
 * This lives in pcbnew rather than in the panel because the panel is part of
 * kicommon, which cannot link against pcbnew.  Keeping the whole surface here
 * also keeps the KiCad-side patch to a single line: everything Inductive adds to
 * the board editor is in this one translation unit, in Inductive's own tree.
 *
 * Called once, from the PCB_EDIT_FRAME constructor, on the UI thread.
 */
void InductiveInstallBoardTools( PCB_EDIT_FRAME* aFrame, INDUCTIVE_PANEL* aPanel );

/// Gerbers, drill files, BOM and placement file for a JLCPCB assembly order, in a
/// "jlcpcb" folder beside the saved board. Needs no editor window.
bool ExportJlcpcb( BOARD* aBoard, wxString& aResult );

/// Why the board should not go to a fab yet (unrouted connections, DRC errors the user
/// has not excluded), from its current markers; empty when it can. Export tools run DRC
/// and then this, and refuse unless asked to export anyway. Findings between two items
/// of one footprint (how a library part is drawn) never block; aNote says which.
wxString FabBlockers( BOARD* aBoard, wxString* aNote = nullptr );

nlohmann::json InductiveBoardWorkflowSnapshot(BOARD*, bool checkpoint);

#endif  // INDUCTIVE_BOARD_TOOLS_H
