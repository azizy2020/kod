/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */

#include "WinSystemAmlogic.h"

#include <string.h>
#include <float.h>

#include "ServiceBroker.h"
#include "cores/RetroPlayer/process/amlogic/RPProcessInfoAmlogic.h"
#include "cores/RetroPlayer/rendering/VideoRenderers/RPRendererOpenGLES.h"
#include "cores/VideoPlayer/DVDCodecs/Video/DVDVideoCodecAmlogic.h"
#include "cores/VideoPlayer/VideoRenderers/LinuxRendererGLES.h"
#include "cores/VideoPlayer/VideoRenderers/HwDecRender/RendererAML.h"
// AESink Factory
#include "cores/AudioEngine/AESinkFactory.h"
#include "cores/AudioEngine/Sinks/AESinkALSA.h"
#include "cores/AudioEngine/Sinks/AESinkPULSE.h"
#include "windowing/GraphicContext.h"
#include "windowing/Resolution.h"
#include "platform/linux/powermanagement/LinuxPowerSyscall.h"
#include "platform/linux/ScreenshotSurfaceAML.h"
#include "settings/DisplaySettings.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "guilib/DispResource.h"
#include "utils/AMLUtils.h"
#include "utils/log.h"
#include "utils/SysfsUtils.h"
#include "threads/SingleLock.h"

#include <linux/fb.h>

#include "system_egl.h"

using namespace KODI;

CWinSystemAmlogic::CWinSystemAmlogic() :
  m_libinput(new CLibInputHandler)
{
  const char *env_framebuffer = getenv("FRAMEBUFFER");

  // default to framebuffer 0
  m_framebuffer_name = "fb0";
  if (env_framebuffer)
  {
    std::string framebuffer(env_framebuffer);
    std::string::size_type start = framebuffer.find("fb");
    m_framebuffer_name = framebuffer.substr(start);
  }

  m_nativeDisplay = EGL_NO_DISPLAY;
  m_nativeWindow = static_cast<EGLNativeWindowType>(NULL);

  m_displayWidth = 0;
  m_displayHeight = 0;

  m_stereo_mode = RENDER_STEREO_MODE_OFF;
  m_delayDispReset = false;

  aml_permissions();

  // Register sink
  AE::CAESinkFactory::ClearSinks();
  CAESinkALSA::Register();
  CAESinkPULSE::Register();
  m_libinput->Start();
}

CWinSystemAmlogic::~CWinSystemAmlogic()
{
  if(m_nativeWindow)
  {
    m_nativeWindow = static_cast<EGLNativeWindowType>(NULL);
  }
}

bool CWinSystemAmlogic::InitWindowSystem()
{
  const std::shared_ptr<CSettings> settings = CServiceBroker::GetSettingsComponent()->GetSettings();

  if (settings->GetBool(CSettings::SETTING_COREELEC_AMLOGIC_NOISEREDUCTION))
  {
     CLog::Log(LOGDEBUG, "CWinSystemAmlogic::InitWindowSystem -- disabling noise reduction");
     SysfsUtils::SetString("/sys/module/di/parameters/nr2_en", "0");
  }

  int sdr2hdr = settings->GetInt(CSettings::SETTING_COREELEC_AMLOGIC_SDR2HDR);
  if (sdr2hdr != 0) // Default is Off (0)
  {
    CLog::Log(LOGDEBUG, "CWinSystemAmlogic::InitWindowSystem -- setting sdr2hdr mode to %d", sdr2hdr);
    SysfsUtils::SetInt("/sys/module/am_vecm/parameters/sdr_mode", sdr2hdr);
  }

  int hdr2sdr = settings->GetInt(CSettings::SETTING_COREELEC_AMLOGIC_HDR2SDR);
  if (hdr2sdr != 2) // Default is Auto (2)
  {
    CLog::Log(LOGDEBUG, "CWinSystemAmlogic::InitWindowSystem -- setting hdr2sdr mode to %d", hdr2sdr);
    SysfsUtils::SetInt("/sys/module/am_vecm/parameters/hdr_mode", hdr2sdr);
  }

  std::string attr = "";
  SysfsUtils::GetString("/sys/class/amhdmitx/amhdmitx0/attr", attr);
  //We delay writing attr until everything is done with it to avoid multiple display resets.
  if (CServiceBroker::GetSettingsComponent()->GetSettings()->GetBool(CSettings::SETTING_COREELEC_AMLOGIC_FORCE422))
  {
     CLog::Log(LOGDEBUG, "CWinSystemAmlogic::InitWindowSystem -- Setting 422 output");
     if (attr.find("444") != std::string::npos ||
         attr.find("422") != std::string::npos ||
         attr.find("420") != std::string::npos)
       attr.replace(attr.find("4"),3,"422");
     else
       attr.append("422");
  }
  if (CServiceBroker::GetSettingsComponent()->GetSettings()->GetBool(CSettings::SETTING_COREELEC_AMLOGIC_LIMIT8BIT))
  {
     CLog::Log(LOGDEBUG, "CWinSystemAmlogic::InitWindowSystem -- Limiting display to 8bit colour depth");
     if (attr.find("10bit") != std::string::npos)
       attr.replace(attr.find("10bit"),5,"8bit");
     else if (attr.find("12bit") != std::string::npos)
       attr.replace(attr.find("12bit"),5,"8bit");
     else if (attr.find("8bit") != std::string::npos)
       ; //do nothing
     else
       attr.append("8bit");
  }
  if (CServiceBroker::GetSettingsComponent()->GetSettings()->GetBool(CSettings::SETTING_COREELEC_AMLOGIC_LIMIT8BIT) ||
      CServiceBroker::GetSettingsComponent()->GetSettings()->GetBool(CSettings::SETTING_COREELEC_AMLOGIC_FORCE422))
  {
     //attr.append("now");
     SysfsUtils::SetString("/sys/class/amhdmitx/amhdmitx0/attr", attr.c_str());
  }

  m_nativeDisplay = EGL_DEFAULT_DISPLAY;

  CDVDVideoCodecAmlogic::Register();
  CLinuxRendererGLES::Register();
  RETRO::CRPProcessInfoAmlogic::Register();
  RETRO::CRPProcessInfoAmlogic::RegisterRendererFactory(new RETRO::CRendererFactoryOpenGLES);
  CRendererAML::Register();
  CScreenshotSurfaceAML::Register();

  if (aml_get_cpufamily_id() <= AML_GXL)
    aml_set_framebuffer_resolution(1920, 1080, m_framebuffer_name);

  // kill a running animation
  CLog::Log(LOGDEBUG,"CWinSystemAmlogic: Sending SIGUSR1 to 'splash-image'\n");
  std::system("killall -s SIGUSR1 splash-image &> /dev/null");

  return CWinSystemBase::InitWindowSystem();
}

bool CWinSystemAmlogic::DestroyWindowSystem()
{
  return true;
}

bool CWinSystemAmlogic::CreateNewWindow(const std::string& name,
                                    bool fullScreen,
                                    RESOLUTION_INFO& res)
{
  RESOLUTION_INFO current_resolution;
  current_resolution.iWidth = current_resolution.iHeight = 0;
  RENDER_STEREO_MODE stereo_mode = CServiceBroker::GetWinSystem()->GetGfxContext().GetStereoMode();

  m_nWidth        = res.iWidth;
  m_nHeight       = res.iHeight;
  m_displayWidth  = res.iScreenWidth;
  m_displayHeight = res.iScreenHeight;
  m_fRefreshRate  = res.fRefreshRate;

  if ((m_bWindowCreated && aml_get_native_resolution(&current_resolution)) &&
    current_resolution.iWidth == res.iWidth && current_resolution.iHeight == res.iHeight &&
    current_resolution.iScreenWidth == res.iScreenWidth && current_resolution.iScreenHeight == res.iScreenHeight &&
    m_bFullScreen == fullScreen && current_resolution.fRefreshRate == res.fRefreshRate &&
    (current_resolution.dwFlags & D3DPRESENTFLAG_MODEMASK) == (res.dwFlags & D3DPRESENTFLAG_MODEMASK) &&
    m_stereo_mode == stereo_mode)
  {
    CLog::Log(LOGDEBUG, "CWinSystemEGL::CreateNewWindow: No need to create a new window");
    return true;
  }

  int delay = CServiceBroker::GetSettingsComponent()->GetSettings()->GetInt("videoscreen.delayrefreshchange");
  if (delay > 0)
  {
    m_delayDispReset = true;
    m_dispResetTimer.Set(delay * 100);
  }

  {
    CSingleLock lock(m_resourceSection);
    for (std::vector<IDispResource *>::iterator i = m_resources.begin(); i != m_resources.end(); ++i)
    {
      (*i)->OnLostDisplay();
    }
  }

  m_stereo_mode = stereo_mode;
  m_bFullScreen = fullScreen;

#ifdef _FBDEV_WINDOW_H_
  fbdev_window *nativeWindow = new fbdev_window;
  nativeWindow->width = res.iWidth;
  nativeWindow->height = res.iHeight;
  m_nativeWindow = static_cast<EGLNativeWindowType>(nativeWindow);
#endif

  aml_set_native_resolution(res, m_framebuffer_name, stereo_mode);

  if (!m_delayDispReset)
  {
    CSingleLock lock(m_resourceSection);
    // tell any shared resources
    for (std::vector<IDispResource *>::iterator i = m_resources.begin(); i != m_resources.end(); ++i)
    {
      (*i)->OnResetDisplay();
    }
  }

  return true;
}

bool CWinSystemAmlogic::DestroyWindow()
{
  m_nativeWindow = static_cast<EGLNativeWindowType>(NULL);

  return true;
}

void CWinSystemAmlogic::UpdateResolutions()
{
  CWinSystemBase::UpdateResolutions();

  RESOLUTION_INFO resDesktop, curDisplay;
  std::vector<RESOLUTION_INFO> resolutions;

  if (!aml_probe_resolutions(resolutions) || resolutions.empty())
  {
    CLog::Log(LOGWARNING, "%s: ProbeResolutions failed.",__FUNCTION__);
  }

  /* ProbeResolutions includes already all resolutions.
   * Only get desktop resolution so we can replace xbmc's desktop res
   */
  if (aml_get_native_resolution(&curDisplay))
  {
    resDesktop = curDisplay;
  }

  RESOLUTION ResDesktop = RES_INVALID;
  RESOLUTION res_index  = RES_DESKTOP;

  for (size_t i = 0; i < resolutions.size(); i++)
  {
    // if this is a new setting,
    // create a new empty setting to fill in.
    if ((int)CDisplaySettings::GetInstance().ResolutionInfoSize() <= res_index)
    {
      RESOLUTION_INFO res;
      CDisplaySettings::GetInstance().AddResolutionInfo(res);
    }

    CServiceBroker::GetWinSystem()->GetGfxContext().ResetOverscan(resolutions[i]);
    CDisplaySettings::GetInstance().GetResolutionInfo(res_index) = resolutions[i];

    CLog::Log(LOGINFO, "Found resolution %d x %d with %d x %d%s @ %f Hz\n",
      resolutions[i].iWidth,
      resolutions[i].iHeight,
      resolutions[i].iScreenWidth,
      resolutions[i].iScreenHeight,
      resolutions[i].dwFlags & D3DPRESENTFLAG_INTERLACED ? "i" : "",
      resolutions[i].fRefreshRate);

    if(resDesktop.iWidth == resolutions[i].iWidth &&
       resDesktop.iHeight == resolutions[i].iHeight &&
       resDesktop.iScreenWidth == resolutions[i].iScreenWidth &&
       resDesktop.iScreenHeight == resolutions[i].iScreenHeight &&
       (resDesktop.dwFlags & D3DPRESENTFLAG_MODEMASK) == (resolutions[i].dwFlags & D3DPRESENTFLAG_MODEMASK) &&
       fabs(resDesktop.fRefreshRate - resolutions[i].fRefreshRate) < FLT_EPSILON)
    {
      ResDesktop = res_index;
    }

    res_index = (RESOLUTION)((int)res_index + 1);
  }

  // set RES_DESKTOP
  if (ResDesktop != RES_INVALID)
  {
    CLog::Log(LOGINFO, "Found (%dx%d%s@%f) at %d, setting to RES_DESKTOP at %d",
      resDesktop.iWidth, resDesktop.iHeight,
      resDesktop.dwFlags & D3DPRESENTFLAG_INTERLACED ? "i" : "",
      resDesktop.fRefreshRate,
      (int)ResDesktop, (int)RES_DESKTOP);

    CDisplaySettings::GetInstance().GetResolutionInfo(RES_DESKTOP) = CDisplaySettings::GetInstance().GetResolutionInfo(ResDesktop);
  }
}

bool CWinSystemAmlogic::Hide()
{
  return false;
}

bool CWinSystemAmlogic::Show(bool show)
{
  std::string blank_framebuffer = "/sys/class/graphics/" + m_framebuffer_name + "/blank";
  SysfsUtils::SetInt(blank_framebuffer.c_str(), show ? 0 : 1);
  return true;
}

void CWinSystemAmlogic::Register(IDispResource *resource)
{
  CSingleLock lock(m_resourceSection);
  m_resources.push_back(resource);
}

void CWinSystemAmlogic::Unregister(IDispResource *resource)
{
  CSingleLock lock(m_resourceSection);
  std::vector<IDispResource*>::iterator i = find(m_resources.begin(), m_resources.end(), resource);
  if (i != m_resources.end())
    m_resources.erase(i);
}
