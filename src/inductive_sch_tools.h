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

#ifndef INDUCTIVE_SCH_TOOLS_H
#define INDUCTIVE_SCH_TOOLS_H

#include <functional>
#include <memory>
#include <vector>
#include <nlohmann/json_fwd.hpp>

class SCHEMATIC;
class SCH_ITEM;
class SCH_SYMBOL;
class SCH_SCREEN;
class SCH_SHEET;
struct INDUCTIVE_CIRCUIT_EDIT;
class wxString;
class SCH_EDIT_FRAME;
class INDUCTIVE_PANEL;

/**
 * Give the Inductive sidebar the schematic editor's capabilities.
 *
 * The counterpart of InductiveInstallBoardTools(): registers the document
 * summary, the slash commands, and the tools the model may call against the open
 * schematic.  Lives in eeschema because kicommon, where the panel lives, cannot
 * link against it.
 *
 * Called once, from the SCH_EDIT_FRAME constructor, on the UI thread.
 */
void InductiveInstallSchematicTools( SCH_EDIT_FRAME* aFrame, INDUCTIVE_PANEL* aPanel );

nlohmann::json InductiveReviewSnapshot(SCHEMATIC&);

/// Read the actual selected items on the active sheet, promoting symbol pins/fields.
nlohmann::json InductiveSchematicSelection( SCHEMATIC&, const std::vector<SCH_ITEM*>& );

std::shared_ptr<INDUCTIVE_CIRCUIT_EDIT> InductivePreviewCircuitEdit( SCHEMATIC&, const nlohmann::json&, bool aVisual = true );
nlohmann::json InductiveCircuitEditSummary( const std::shared_ptr<INDUCTIVE_CIRCUIT_EDIT>& );
void InductiveValidateCircuitEdit( SCHEMATIC&, const std::shared_ptr<INDUCTIVE_CIRCUIT_EDIT>& );

std::unique_ptr<SCHEMATIC> InductiveStageCircuitEdit( SCHEMATIC&, const std::shared_ptr<INDUCTIVE_CIRCUIT_EDIT>& );

std::unique_ptr<SCH_SHEET> InductivePrepareAttachedSchematic( SCHEMATIC&, const nlohmann::json&, wxString& );

std::unique_ptr<SCH_SHEET> InductiveReadAttachedSchematic( SCHEMATIC*, const nlohmann::json& );

/// Whether a symbol becomes a footprint on the board: not a power symbol (GND, PWR_FLAG)
/// and not excluded from the board.
bool InductiveSymbolOnBoard( const SCH_SYMBOL* aSymbol );

/// search_symbols with no library: every installed symbol, by name, description or keyword.
wxString InductiveSearchSymbols( const wxString& aQuery );

/// Installed footprints a symbol's footprint filters accept, as get_symbol_definition lists them.
wxString InductiveSuggestedFootprints( const wxArrayString& aFilters );

std::vector<std::unique_ptr<SCH_ITEM>> InductiveBuildCircuitItems(
        SCHEMATIC* aSchematic, const nlohmann::json& aArgs, wxString& aResult );

nlohmann::json InductiveSchematicWorkflowSnapshot(SCHEMATIC&, bool checkpoint);

/// The BOM grouped for /v1/parts/source: refs, value, footprint, lib_id, shared LCSC, rail_v.
nlohmann::json InductiveBomForSourcing( SCHEMATIC& );

/// Record a /v1/parts/source answer's picked lines as LCSC/MPN fields.
nlohmann::json InductiveApplySourcedParts( SCHEMATIC&, const nlohmann::json& aResponse, bool aOverwrite,
                                           const std::function<void( SCH_SYMBOL*, SCH_SCREEN* )>& aModify );

#endif  // INDUCTIVE_SCH_TOOLS_H
