/**
  * Touhou Community Reliant Automatic Patcher
  * Cheap command-line patch stack configuration tool
  */

#include <thcrap.h>
#include "configure.h"

typedef struct {
	DWORD size_min;
	DWORD size_max;
	json_t *versions;
	json_t *found;
	json_t *result;
	DWORD max_threads;
	DWORD cur_threads;
	DWORD prompt_countdown;
	CRITICAL_SECTION cs_result;
	CRITICAL_SECTION cs_count;
} search_state_t;

static search_state_t state;

int SearchCheckExe(char *local_dir, WIN32_FIND_DATAA *w32fd)
{
	json_t *ver = identify_by_size(w32fd->nFileSizeLow, state.versions);
	if(!ver) {
		return 0;
	} else {
		STRLEN_DEC(local_dir);
		size_t exe_fn_len = local_dir_len + strlen(w32fd->cFileName) + 2;
		VLA(char, exe_fn, exe_fn_len);
		const char *key;
		json_t *game_val;

		strncpy(exe_fn, local_dir, local_dir_len);
		exe_fn[local_dir_len - 1] = 0;
		strcat(exe_fn, w32fd->cFileName);
		str_slash_normalize(exe_fn);

		ver = identify_by_hash(exe_fn, &w32fd->nFileSizeLow, state.versions);
		if(!ver) {
			return 0;
		}

		key = json_array_get_string(ver, 0);

		// Check if user already selected a version of this game in a previous search
		if(json_object_get(state.result, key)) {
			return 0;
		}

		// Alright, found a game!
		// Add it into the result object
		game_val = json_object_get_create(state.found, key, json_object());
		
		EnterCriticalSection(&state.cs_result);
		{
			const char *build = json_array_get_string(ver, 1);
			const char *variety = json_array_get_string(ver, 2);
			size_t id_str_len = 0;
	
			if(build) {
				id_str_len += strlen(build) + 1;
			}
			if(variety) {
				id_str_len += strlen(variety) + 1;
			}
			{
				VLA(char, id_str, id_str_len);
				id_str[0] = 0;
				if(build) {
					strcpy(id_str, build);
				}
				if(variety) {
					strcat(id_str, " ");
					strcat(id_str, variety);
				}
				json_object_set_new(game_val, exe_fn, json_string(id_str));
				VLA_FREE(id_str);
			}
		}
		LeaveCriticalSection(&state.cs_result);
		VLA_FREE(exe_fn);
	}
	return 1;
}


DWORD WINAPI SearchThread(void *param)
{
	HANDLE hFind;
	WIN32_FIND_DATAA w32fd;
	char *param_dir = (char*)param;

	// Add the asterisk
	size_t dir_len = strlen(param_dir) + 2 + 1;
	VLA(char, local_dir, dir_len);
	
	BOOL ret = 1;

	strcpy(local_dir, param_dir);
	strcat(local_dir, "*");

	EnterCriticalSection(&state.cs_count);
	state.cur_threads++;
	LeaveCriticalSection(&state.cs_count);

	// if(!--state.prompt_countdown) {
		// state.prompt_countdown = 100;
	// }
	SAFE_FREE(param_dir);
	hFind = FindFirstFile(local_dir, &w32fd);
	while( (hFind != INVALID_HANDLE_VALUE) && ret ) {
		if(
			!strcmp(w32fd.cFileName, ".") ||
			!strcmp(w32fd.cFileName, "..")
		) {
			ret = FindNextFile(hFind, &w32fd);
			continue;
		}
		local_dir[dir_len - 3] = 0;
		
		if(w32fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
			DWORD thread_id;
			size_t cur_file_len = dir_len + strlen(w32fd.cFileName) + 2;
			char *cur_file = (char*)malloc(cur_file_len * sizeof(char));

			strncpy(cur_file, local_dir, dir_len);
			strcat(cur_file, w32fd.cFileName);
			PathAddBackslashA(cur_file);

			// Execute the function normally if we're above the thread limit
			if(state.cur_threads > state.max_threads) {
				SearchThread(cur_file);
			} else {
				CreateThread(NULL, 0, SearchThread, cur_file, 0, &thread_id);
			}
		} else if(
			(PathMatchSpec(w32fd.cFileName, "*.exe")) &&
			(w32fd.nFileSizeLow >= state.size_min) &&
			(w32fd.nFileSizeLow <= state.size_max)
		) {
			SearchCheckExe(local_dir, &w32fd);
		}
		ret = FindNextFile(hFind, &w32fd);
	}
	FindClose(hFind);
	EnterCriticalSection(&state.cs_count);
	state.cur_threads--;
	LeaveCriticalSection(&state.cs_count);
	VLA_FREE(local_dir);
	return 0;
}

void LaunchSearchThread(const char *dir)
{
	DWORD thread_id;
	HANDLE initial_thread;

	// Freed inside the thread
	char *cur_dir = strdup(dir);
	initial_thread = CreateThread(NULL, 0, SearchThread, cur_dir, 0, &thread_id);
	WaitForSingleObject(initial_thread, INFINITE);
}

json_t* SearchForGames(const char *dir, json_t *games_in)
{
	const char *versions_js_fn = "versions.js";
	json_t *sizes;

	const char *key;
	json_t *val;

	/*wchar_t *cur_dir = (wchar_t*)malloc(MAX_PATH);
	wcscpy(cur_dir, dir);
	str_slash_normalize(cur_dir);*/

	state.versions = stack_json_resolve(versions_js_fn, NULL);
	if(!state.versions) {
		log_printf(
			"ERROR: No version definition file (%s) found!\n"
			"Seems as if base_tsa didn't download correctly.\n"
			"Try deleting the thpatch directory and running this program again.\n", versions_js_fn
		);
		return NULL;
	}
	sizes = json_object_get(state.versions, "sizes");
	// Error...

	state.size_min = 0xFFFFFFFF;
	state.size_max = 0;
	state.cur_threads = 0;
	state.max_threads = 0;
	state.prompt_countdown = 1;
	state.found = json_object();
	state.result = games_in ? games_in : json_object();
	InitializeCriticalSection(&state.cs_result);
	InitializeCriticalSection(&state.cs_count);

	// Get file size limits
	json_object_foreach(sizes, key, val) {
		size_t cur_size = atoi(key);

		state.size_min = min(cur_size, state.size_min);
		state.size_max = max(cur_size, state.size_max);
	}

	if(dir && dir[0]) {
		LaunchSearchThread(dir);
	} else {
		char drive_strings[512];
		char *p = drive_strings;

		GetLogicalDriveStringsA(512, drive_strings);
		while(p && p[0]) {
			UINT drive_type = GetDriveTypeA(p);
			if(
				(drive_type != DRIVE_CDROM) &&
				(p[0] != 'A') &&
				(p[0] != 'a')
			) {
				LaunchSearchThread(p);
			}
			p += strlen(p) + 1;
		}
	}
	while(state.cur_threads) {
		Sleep(100);
	}
	
	DeleteCriticalSection(&state.cs_result);
	DeleteCriticalSection(&state.cs_count);
	json_decref(state.versions);
	return state.found;
}
