#pragma once

/*
 * starlight-sys :: injector/src/byovd.h
 *
 * BYOVD (Bring Your Own Vulnerable Driver) loader.
 *
 * Handles loading/unloading a signed vulnerable driver via the
 * Windows Service Control Manager (SCM). The driver is loaded as
 * a legitimate kernel service - no DSE bypass needed for this step
 * because the driver has a valid signature.
 */

#include <Windows.h>
#include <cstdint>
#include <string>

/* ------------------------------------------------------------------ */
/*  BYOVD Driver Loader                                                */
/* ------------------------------------------------------------------ */

class ByovdLoader {
public:
    ByovdLoader();
    ~ByovdLoader();
    
    /*
     * LoadDriver - Register and start the vulnerable driver as a service.
     *
     * Parameters:
     *   driverPath   - Full path to the .sys file
     *   serviceName  - Name for the Windows service
     *
     * This uses the Service Control Manager to:
     * 1. Create a new service of type SERVICE_KERNEL_DRIVER
     * 2. Start the service (which loads the .sys into kernel)
     *
     * The driver must have a valid Authenticode signature.
     * It will pass DSE because it's properly signed.
     */
    bool LoadDriver(const std::wstring& driverPath,
                    const std::wstring& serviceName);
    
    /*
     * UnloadDriver - Stop and remove the vulnerable driver service.
     *
     * Called after the manual mapper has finished, to reduce the
     * attack footprint. The unsigned driver is already in kernel
     * memory at this point and doesn't need the vulnerable driver.
     */
    bool UnloadDriver();
    
    /*
     * IsLoaded - Check if the driver is currently loaded.
     */
    bool IsLoaded() const { return m_loaded; }
    
    /*
     * GetDeviceName - Returns the device name for opening with CreateFile.
     */
    const std::wstring& GetDeviceName() const { return m_deviceName; }
    
private:
    SC_HANDLE       m_scManager;
    SC_HANDLE       m_service;
    std::wstring    m_serviceName;
    std::wstring    m_deviceName;
    bool            m_loaded;
};
