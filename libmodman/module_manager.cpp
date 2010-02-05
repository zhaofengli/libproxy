/*******************************************************************************
 * libmodman - A library for extending applications
 * Copyright (C) 2009 Nathaniel McCallum <nathaniel@natemccallum.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 ******************************************************************************/

#include <algorithm>

#include <sys/stat.h> // For stat()
#ifdef WIN32
#include <windows.h>
#else
#include <dlfcn.h>  // For dlopen(), etc...
#include <dirent.h> // For opendir(), readdir(), closedir()
#endif

#include "module_manager.hpp"

#include <cstdio>

#ifdef WIN32
#define pdlmtype HMODULE
#define pdlopen(filename) LoadLibrary(filename)
#define pdlopenl(filename) LoadLibraryEx(filename, NULL, DONT_RESOLVE_DLL_REFERENCES)
#define pdlsym GetProcAddress
#define pdlclose(module) FreeLibrary((pdlmtype) module)
/*static std::string pdlerror() {
	std::string e;
	LPTSTR msg;

	FormatMessage(
			FORMAT_MESSAGE_ALLOCATE_BUFFER |FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
			NULL,
			GetLastError(),
			MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
			(LPTSTR) &msg,
			0,
			NULL);
	e = std::string(msg);
    LocalFree(msg);
    return e;
}*/
static pdlmtype pdlreopen(pdlmtype module) {
	char tmp[4096];
	if (!module) return NULL;
	DWORD i = GetModuleFileName(module, tmp, 4096);
	pdlclose(module);
	return (i == 0 || i == 4096) ? NULL : pdlopen(tmp);
}
#else
#define pdlmtype void*
#define pdlopen(filename) dlopen(filename, RTLD_NOW | RTLD_LOCAL)
#define pdlopenl(filename) dlopen(filename, RTLD_LAZY | RTLD_LOCAL)
#define pdlsym dlsym
#define pdlclose(module) dlclose((pdlmtype) module)
//static std::string pdlerror() { return dlerror(); }
static pdlmtype pdlreopen(pdlmtype module) { return module; }
#endif

#define _str(s) #s
#define __str(s) _str(s)

using namespace com::googlecode::libmodman;

module_manager::~module_manager() {
	// Free all extensions
	for (map<string, vector<base_extension*> >::iterator i=this->extensions.begin() ; i != this->extensions.end() ; i++) {
		for (vector<base_extension*>::iterator j=i->second.begin() ; j != i->second.end() ; j++)
			delete *j;
		i->second.clear();
	}
	this->extensions.clear();

	// Free all modules
	for (set<void*>::iterator i=this->modules.begin() ; i != this->modules.end() ; i++)
		pdlclose(*i);
	this->modules.clear();
}

bool module_manager::load_file(string filename, bool symbreq) {
	// Stat the file to make sure it is a file
	struct stat st;
	if (stat(filename.c_str(), &st) != 0) return false;
	if ((st.st_mode & S_IFMT) != S_IFREG) return false;

	// Open the module without doing any symbol resolution
	pdlmtype dlobj = pdlopenl(filename.c_str());
	if (!dlobj) return false;
	bool lazyload = true;

	// If we have already loaded this module, return true
	if (this->modules.find((void*) dlobj) != this->modules.end()) {
		pdlclose(dlobj);
		return true;
	}

on_reload:
	// Get the module_info struct
	module* mi = (module*) pdlsym(dlobj, __str(MM_MODULE_NAME));
	if (!mi) {
		pdlclose(dlobj);
		return false;
	}

	bool loaded = false;
	for (unsigned int i=0 ; mi[i].vers == MM_MODULE_VERSION && mi[i].type && mi[i].init ; i++) {
		// Make sure the type is registered
		if (this->extensions.find(mi[i].type) == this->extensions.end()) continue;

		// If this is a singleton and we already have an instance, don't instantiate
		if (this->singletons.find(mi[i].type) != this->singletons.end() &&
			this->extensions[mi[i].type].size() > 0) continue;

#ifndef WIN32
		// If a symbol is defined, we'll search for it in the main process
		if (mi[i].symb) {
			// Open the main process
			pdlmtype thisproc = pdlopen(NULL);
			if (!thisproc && symbreq) continue;

			// Try to find the symbol in the main process
			if (!pdlsym(thisproc, mi[i].symb)) {
				// If the symbol is not found and the symbol is required, continue
				if (symbreq) {
					pdlclose(thisproc);
					continue;
				}

				// If the symbol is not found and not required, we'll load
				// only if there are no other modules of this type
				if (this->extensions[mi[i].type].size() > 0) {
					pdlclose(thisproc);
					continue;
				}
			}
			pdlclose(thisproc);
		}
#endif

		if (lazyload) {
			// Reload the module (not in lazy mode)
			dlobj = pdlreopen(dlobj);
			if (!dlobj) return false;
			lazyload = false;
			goto on_reload;
		}

		// If our execution test succeeds, call init()
		if (mi[i].test()) {
			base_extension** extensions = mi[i].init();
			if (extensions) {
				// init() returned extensions we need to register
				loaded = true;
				for (unsigned int j=0 ; extensions[j] ; j++)
					this->extensions[mi[i].type].push_back(extensions[j]);
				delete extensions;
			}
		}
	}

	// We didn't load this module, so exit
	if (!loaded) {
		pdlclose(dlobj);
		return false;
	}

	// Add the dlobject to our known modules
	this->modules.insert((void*) dlobj);

	// Yay, we did it!
	return true;
}

bool module_manager::load_dir(string dirname, bool symbreq) {
	vector<string> files;

#ifdef WIN32
	WIN32_FIND_DATA fd;
	HANDLE search;

	string srch = dirname + "\\" + "*";
	search = FindFirstFile(srch.c_str(), &fd);
	if (search != INVALID_HANDLE_VALUE) {
		do {
			files.push_back(dirname + "\\" + fd.cFileName);
		} while (FindNextFile(search, &fd));
		FindClose(search);
	}
#else
	struct dirent *ent;

	DIR *moduledir = opendir(dirname.c_str());
	if (moduledir) {
		while((ent = readdir(moduledir)))
			files.push_back(dirname + "/" + ent->d_name);
		closedir(moduledir);
	}
#endif

	// Perform our load alphabetically
	sort(files.begin(), files.end());

	// Try to do the load
	bool loaded = false;
	for (vector<string>::iterator it = files.begin() ; it != files.end() ; it++)
		loaded = this->load_file(*it, symbreq) || loaded;
	return loaded;
}