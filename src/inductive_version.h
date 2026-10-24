/*
 * This program source code file is part of Inductive, built on KiCad.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 */

#ifndef INDUCTIVE_VERSION_H
#define INDUCTIVE_VERSION_H

/**
 * The Inductive release, as people and the server see it (not KiCad's version).
 * tools/install-app.sh writes it into every bundle's Info.plist and the DMG's
 * name; the editors send it as X-Inductive-Version, so the server can turn away a
 * release too old for its API with a download link (MIN_CLIENT_VERSION).
 */
#define INDUCTIVE_VERSION "1.1.0"

#endif
