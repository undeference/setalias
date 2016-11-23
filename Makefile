# This file is part of setalias
# Copyright © 2016 M. Kristall
#
# This program is free software; you can redistribute it and/or modify it under
# the terms of version 2 of the GNU General Public License as published by the
# Free Software Foundation.
#
# This program is distributed in the hope that it will be useful, but WITHOUT
# ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
# FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
# details.
#
# You should have received a copy of version 2 of the GNU General Public
# License with this program; if not, write to the Free Software Foundation, Inc
# 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA

CC=gcc
RM=rm -f
INSTALL=install

OPTIMIZE_FLAGS=-DNDEBUG -O3
DEBUG_FLAGS=-g3
WARNING_FLAGS=-Wall

DEFS=
CFLAGS=

EXE=setalias
EXEDBG=$(EXE)dbg
INSTALL_PATH=/usr/bin

ifdef ALIASFILE
	DEFS+= -DALIASFILE=\"$(ALIASFILE)\"
endif
ifdef NEWALIASES
	DEFS+= -DNEWALIASES=\"$(NEWALIASES)\"
endif

CFLAGS+= $(DEFS)

.PHONY default
default: release

.PHONY debug
debug:
	$(CC) $(CFLAGS) $(DEBUG_FLAGS) $(WARNING_FLAGS) setalias.c -o $(EXEDBG)

.PHONY release
release:
	$(CC) $(CFLAGS) $(OPTIMIZE_FLAGS) $(WARNING_FLAGS) setalias.c -o $(EXE)

.PHONY clean
clean:
	$(RM) $(EXEDBG) $(EXE)

.PHONY install
install:
	$(RM) $(INSTALL_PATH)/$(EXE)
	$(INSTALL) -g root -o root -m 4755 -s $(EXE) $(INSTALL_PATH)
