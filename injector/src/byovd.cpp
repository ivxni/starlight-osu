/*
 * starlight-sys :: injector/src/byovd.cpp
 *
 * Driver loading via Service Control Manager.
 */

#include "byovd.h"
#include <cstdio>

/* ------------------------------------------------------------------ */
/*  Constructor / Destructor                                           */
/* ------------------------------------------------------------------ */

ByovdLoader::ByovdLoader()
    : m_scManager(NULL)
    , m_service(NULL)
    , m_loaded(false)
{
}

ByovdLoader::~ByovdLoader()
{
    if (m_loaded)
        UnloadDriver();
    
    if (m_service)
        CloseServiceHandle(m_service);
    if (m_scManager)
        CloseServiceHandle(m_scManager);
}

/* ------------------------------------------------------------------ */
/*  Load Driver                                                        */
/* ------------------------------------------------------------------ */

bool ByovdLoader::LoadDriver(const std::wstring& driverPath,
                              const std::wstring& serviceName)
{
    m_serviceName = serviceName;

    /* Step 1: Open SCM */
    m_scManager = OpenSCManagerW(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (!m_scManager)
    {
        printf("[!] SCM access denied (error %lu)\n", GetLastError());
        return false;
    }
    
    /* Step 2: Create service */
    m_service = CreateServiceW(
        m_scManager,
        serviceName.c_str(),
        serviceName.c_str(),
        SERVICE_ALL_ACCESS,
        SERVICE_KERNEL_DRIVER,
        SERVICE_DEMAND_START,
        SERVICE_ERROR_IGNORE,
        driverPath.c_str(),
        NULL, NULL, NULL, NULL, NULL
    );
    
    if (!m_service)
    {
        DWORD error = GetLastError();
        if (error == ERROR_SERVICE_EXISTS)
        {
            m_service = OpenServiceW(
                m_scManager,
                serviceName.c_str(),
                SERVICE_ALL_ACCESS
            );
        }
        else if (error == ERROR_SERVICE_MARKED_FOR_DELETE)
        {
            /* Previous service still being cleaned up - wait and retry */
            Sleep(2000);
            m_service = CreateServiceW(
                m_scManager,
                serviceName.c_str(),
                serviceName.c_str(),
                SERVICE_ALL_ACCESS,
                SERVICE_KERNEL_DRIVER,
                SERVICE_DEMAND_START,
                SERVICE_ERROR_IGNORE,
                driverPath.c_str(),
                NULL, NULL, NULL, NULL, NULL
            );
            if (!m_service && GetLastError() == ERROR_SERVICE_EXISTS) {
                m_service = OpenServiceW(
                    m_scManager,
                    serviceName.c_str(),
                    SERVICE_ALL_ACCESS
                );
            }
        }
        
        if (!m_service)
        {
            printf("[!] Service creation failed (error %lu)\n",
                   GetLastError());
            return false;
        }
    }
    
    /* Step 3: Start service (loads .sys into kernel) */
    if (!StartServiceW(m_service, 0, NULL))
    {
        DWORD error = GetLastError();
        if (error == ERROR_SERVICE_ALREADY_RUNNING)
        {
            /* already running - fine */
        }
        else
        {
            printf("[!] Service start failed (error %lu)\n", error);
            DeleteService(m_service);
            return false;
        }
    }
    
    m_loaded = true;
    return true;
}

/* ------------------------------------------------------------------ */
/*  Unload Driver                                                      */
/* ------------------------------------------------------------------ */

bool ByovdLoader::UnloadDriver()
{
    if (!m_loaded || !m_service)
        return false;
    
    SERVICE_STATUS status = {};
    ControlService(m_service, SERVICE_CONTROL_STOP, &status);
    Sleep(500);
    DeleteService(m_service);
    
    CloseServiceHandle(m_service);
    m_service = NULL;
    m_loaded  = false;
    
    return true;
}
