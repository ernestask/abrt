/*
    Copyright (C) 2009  Jiri Moskovcak (jmoskovc@redhat.com)
    Copyright (C) 2009  RedHat inc.

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
    */

#include "abrtlib.h"
#include "Daemon.h"
#include "ABRTException.h"
#include "DebugDump.h"
#include "CrashWatcher.h"

void CCrashWatcher::Status(const char *pMessage, const char* peer, uint64_t pJobID)
{
    VERB1 log("Update('%s'): %s", peer, pMessage);
    if (g_pCommLayer != NULL)
        g_pCommLayer->Update(pMessage, peer, pJobID);
}

void CCrashWatcher::Warning(const char *pMessage, const char* peer, uint64_t pJobID)
{
    VERB1 log("Warning('%s'): %s", peer, pMessage);
    if (g_pCommLayer != NULL)
        g_pCommLayer->Warning(pMessage, peer, pJobID);
}

CCrashWatcher::CCrashWatcher()
{
}

CCrashWatcher::~CCrashWatcher()
{
}

vector_map_crash_data_t GetCrashInfos(const char *pUID)
{
    vector_map_crash_data_t retval;
    log("Getting crash infos...");
    try
    {
        vector_pair_string_string_t UUIDsUIDs;
        UUIDsUIDs = GetUUIDsOfCrash(pUID);

        unsigned int ii;
        for (ii = 0; ii < UUIDsUIDs.size(); ii++)
        {
            mw_result_t res;
            map_crash_data_t info;
            const char *uuid = UUIDsUIDs[ii].first.c_str();
            const char *uid = UUIDsUIDs[ii].second.c_str();

            res = FillCrashInfo(uuid, uid, info);
            switch (res)
            {
                case MW_OK:
                    retval.push_back(info);
                    break;
                case MW_ERROR:
                    error_msg("Dump directory for UUID %s doesn't exist or misses crucial files, deleting", uuid);
                    /* Deletes both DB record and dump dir */
                    DeleteDebugDump(uuid, uid);
                    break;
                default:
                    break;
            }
        }
    }
    catch (CABRTException& e)
    {
        error_msg("%s", e.what());
    }

    //retval = GetCrashInfos(pUID);
    //Notify("Sent crash info");
    return retval;
}

/*
 * Called in two cases:
 * (1) by StartJob dbus call -> CreateReportThread(), in the thread
 * (2) by CreateReport dbus call
 * In the second case, it finishes quickly, because previous
 * StartJob dbus call already did all the processing, and we just retrieve
 * the result from dump directory, which is fast.
 */
map_crash_data_t CreateReport(const char* pUUID, const char* pUID, int force)
{
    /* FIXME: starting from here, any shared data must be protected with a mutex.
     * For example, CreateCrashReport does:
     * g_pPluginManager->GetDatabase(g_settings_sDatabase.c_str());
     * which is unsafe wrt concurrent updates to g_pPluginManager state.
     */
    map_crash_data_t crashReport;
    mw_result_t res = CreateCrashReport(pUUID, pUID, force, crashReport);
    switch (res)
    {
        case MW_OK:
            break;
        case MW_IN_DB_ERROR:
            error_msg("Can't find crash with UUID %s in database", pUUID);
            break;
        case MW_PLUGIN_ERROR:
            error_msg("Particular analyzer plugin isn't loaded or there is an error within plugin(s)");
            break;
        default:
            error_msg("Corrupted crash with UUID %s, deleting", pUUID);
            DeleteDebugDump(pUUID, pUID);
            break;
    }
    return crashReport;
}

typedef struct thread_data_t {
    pthread_t thread_id;
    char* UUID;
    char* UID;
    int force;
    char* peer;
} thread_data_t;
static void* create_report(void* arg)
{
    thread_data_t *thread_data = (thread_data_t *) arg;

    /* Client name is per-thread, need to set it */
    set_client_name(thread_data->peer);

    try
    {
        log("Creating report...");
        map_crash_data_t crashReport = CreateReport(thread_data->UUID, thread_data->UID, thread_data->force);
        g_pCommLayer->JobDone(thread_data->peer, thread_data->UUID);
    }
    catch (CABRTException& e)
    {
        error_msg("%s", e.what());
    }
    catch (...) {}
    set_client_name(NULL);

    /* free strduped strings */
    free(thread_data->UUID);
    free(thread_data->UID);
    free(thread_data->peer);
    free(thread_data);

    /* Bogus value. pthreads require us to return void* */
    return NULL;
}
int CreateReportThread(const char* pUUID, const char* pUID, int force, const char* pSender)
{
    thread_data_t *thread_data = (thread_data_t *)xzalloc(sizeof(thread_data_t));
    thread_data->UUID = xstrdup(pUUID);
    thread_data->UID = xstrdup(pUID);
    thread_data->force = force;
    thread_data->peer = xstrdup(pSender);

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    int r = pthread_create(&thread_data->thread_id, &attr, create_report, thread_data);
    pthread_attr_destroy(&attr);
    if (r != 0)
    {
        free(thread_data->UUID);
        free(thread_data->UID);
        free(thread_data->peer);
        free(thread_data);
        /* The only reason this may happen is system-wide resource starvation,
         * or ulimit is exceeded (someone floods us with CreateReport() dbus calls?)
         */
        error_msg("Can't create thread");
        return r;
    }
    VERB3 log("Thread %llx created", (unsigned long long)thread_data->thread_id);
    return r;
}


/* Remove dump dir and its DB record */
int DeleteDebugDump(const char *pUUID, const char *pUID)
{
    try
    {
        CDatabase* database = g_pPluginManager->GetDatabase(g_settings_sDatabase.c_str());
        database->Connect();
        database_row_t row = database->GetRow(pUUID, pUID);
        database->DeleteRow(pUUID, pUID);
        database->DisConnect();

        const char *dump_dir = row.m_sDebugDumpDir.c_str();
        if (dump_dir[0] != '\0')
        {
            delete_debug_dump_dir(dump_dir);
            return 0; /* success */
        }
    }
    catch (CABRTException& e)
    {
        error_msg("%s", e.what());
    }
    return -1; /* failure */
}

void DeleteDebugDump_by_dir(const char *dump_dir)
{
    try
    {
        CDatabase* database = g_pPluginManager->GetDatabase(g_settings_sDatabase.c_str());
        database->Connect();
        database->DeleteRows_by_dir(dump_dir);
        database->DisConnect();

        delete_debug_dump_dir(dump_dir);
    }
    catch (CABRTException& e)
    {
        error_msg("%s", e.what());
    }
}
