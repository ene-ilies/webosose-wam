// Copyright (c) 2013-2018 LG Electronics, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// SPDX-License-Identifier: Apache-2.0

#include "WebPageBase.h"

#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>

#include "ApplicationDescription.h"
#include "LogManager.h"
#include "WebAppManagerConfig.h"
#include "WebAppManager.h"
#include "WebPageObserver.h"
#include "WebProcessManager.h"

#include <experimental/filesystem>
#include <sstream>
#include <unordered_map>
#include <WamUtils.h>

// TODO : Check usage of it.
#define CONSOLE_DEBUG(AAA) evaluateJavaScript(std::stringLiteral("console.debug('") + std::stringLiteral(AAA) + std::stringLiteral("');"))

WebPageBase::WebPageBase()
    : m_appDesc(0)
    , m_suspendAtLoad(false)
    , m_isClosing(false)
    , m_isLoadErrorPageFinish(false)
    , m_isLoadErrorPageStart(false)
    , m_enableBackgroundRun(false)
    , m_loadErrorPolicy("default")
    , m_cleaningResources(false)
    , m_isPreload(false)
{
}

WebPageBase::WebPageBase(const std::string& url, ApplicationDescription* desc, const std::string& params)
    : m_appDesc(desc)
    , m_appId(desc->id())
    , m_suspendAtLoad(false)
    , m_isClosing(false)
    , m_isLoadErrorPageFinish(false)
    , m_isLoadErrorPageStart(false)
    , m_enableBackgroundRun(false)
    , m_defaultUrl(url)
    , m_launchParams(params)
    , m_loadErrorPolicy("default")
    , m_cleaningResources(false)
    , m_isPreload(false)
{
}

WebPageBase::~WebPageBase()
{
    LOG_INFO(MSGID_WEBPAGE_CLOSED, 1, PMLOGKS("APP_ID", qPrintable(appId())), "");
}

std::string WebPageBase::launchParams() const
{
    return m_launchParams;
}

void WebPageBase::setLaunchParams(const std::string& params)
{
    m_launchParams = params;
}

void WebPageBase::setApplicationDescription(ApplicationDescription* desc)
{
    m_appDesc = desc;
    setPageProperties();
}

std::string WebPageBase::getIdentifier() const
{
    // If appId is ContainerAppId then it should be ""? Why not just container appid?
    // I think there shouldn't be any chance to be returned container appid even for container base app

    if(appId().empty() || appId() == WebAppManager::instance()->getContainerAppId())
        return "";
    return m_appId;
}

void WebPageBase::load()
{
    LOG_INFO(MSGID_WEBPAGE_LOAD, 2, PMLOGKS("APP_ID", qPrintable(appId())), PMLOGKFV("PID", "%d", getWebProcessPID()), "m_launchParams:%s", qPrintable(m_launchParams));
    /* this function is main load of WebPage : load default url */
    setupLaunchEvent();
    if (!doDeeplinking(m_launchParams)) {
        LOG_INFO(MSGID_WEBPAGE_LOAD, 2, PMLOGKS("APP_ID", qPrintable(appId())), PMLOGKFV("PID", "%d", getWebProcessPID()), "loadDefaultUrl()");
        loadDefaultUrl();
    }
}

void WebPageBase::setupLaunchEvent()
{
    std::stringstream ss;
    ss << "(function() {" << std::endl;
    ss << "    var launchEvent = new CustomEvent('webOSLaunch', { detail: " <<
     (launchParams().empty() ? "{}" : launchParams()) << " });" << std::endl;
    ss << "    if(document.readyState === 'complete') {" << std::endl;
    ss << "        setTimeout(function() {" << std::endl;
    ss << "            document.dispatchEvent(launchEvent);" << std::endl;
    ss << "        }, 1);" << std::endl;
    ss << "    } else {" << std::endl;
    ss << "        document.addEventListener('DOMContentLoaded', function() {" << std::endl;
    ss << "            setTimeout(function() {" << std::endl;
    ss << "                document.dispatchEvent(launchEvent);" << std::endl;
    ss << "            }, 1);" << std::endl;
    ss << "        });" << std::endl;
    ss << "    }" << std::endl;
    ss << "})();";

    addUserScript(ss.str());
}

void WebPageBase::sendLocaleChangeEvent(const std::string& language)
{
    std::stringstream ss;
    ss << "setTimeout(function () {" << std::endl;
    ss << "    var localeEvent=new CustomEvent('webOSLocaleChange');" << std::endl;
    ss << "    document.dispatchEvent(localeEvent);" << std::endl;
    ss << "}, 1);";

    evaluateJavaScript(ss.str());
}

void WebPageBase::cleanResources()
{
    setCleaningResources(true);
}

bool WebPageBase::relaunch(const std::string& launchParams, const std::string& launchingAppId)
{
    resumeWebPagePaintingAndJSExecution();

    // for common webapp relaunch scenario
    // 1. For hosted webapp deeplinking : reload default page
    // 2-1. check progress; to send webOSRelaunch event, then page loading progress should be 100
    // 2-2. Update launchParams
    // 2-3. send webOSRelaunch event

    if(doHostedWebAppRelaunch(launchParams)) {
        LOG_DEBUG("[%s] Hosted webapp; handled", qPrintable(m_appId));
        return true;
    }

    if (!hasBeenShown()){
        LOG_INFO(MSGID_WEBPAGE_RELAUNCH, 2, PMLOGKS("APP_ID", qPrintable(appId())), PMLOGKFV("PID", "%d", getWebProcessPID()), "In Loading(%d%%), Can not handle relaunch now, return false", progress());
        return false;
    }

    setLaunchParams(launchParams);

    // WebPageBase::relaunch handles setting the stageArgs for the launch/relaunch events
    sendRelaunchEvent();
    return true;
}

bool WebPageBase::doHostedWebAppRelaunch(const std::string& launchParams)
{
    /* hosted webapp deeplinking spec
    // legacy case
    "deeplinkingParams":"{ \
        \"contentTarget\" : \"https://www.youtube.com/tv?v=$CONTENTID\" \
    }"
    // webOS4.0 spec
    "deeplinkingParams":"{ \
        \"handledBy\" : \"platform\" || \"app\" || \"default\", \
        \"contentTarget\" : \"https://www.youtube.com/tv?v=$CONTENTID\" \
    }"
    To support backward compatibility, should cover the case not having "handledBy"
    */
    // check deeplinking relaunch condition
#if 0
    QJsonObject obj = QJsonDocument::fromJson(launchParams.toUtf8()).object();
    if (url().scheme() ==  "file"
        || m_defaultUrl.scheme() != "file"
        || obj.isEmpty() /* no launchParams, { }, and this should be check with object().isEmpty()*/
        || obj.value("contentTarget").isUndefined()
        || (m_appDesc && !m_appDesc->handlesDeeplinking())) {
        LOG_INFO(MSGID_WEBPAGE_RELAUNCH, 2, PMLOGKS("APP_ID", qPrintable(appId())), PMLOGKFV("PID", "%d", getWebProcessPID()),
            "%s; NOT enough deeplinking condition; return false", __func__);
        return false;
    }
#else
    QByteArray qBA(launchParams.c_str(), launchParams.length());
    QJsonObject obj = QJsonDocument::fromJson(qBA).object();
    std::unordered_map<std::string, std::string> urlInfo, defaultUrlInfo;
    WamUtils::parseURL(url(), urlInfo);
    WamUtils::parseURL(m_defaultUrl, urlInfo);
    if (urlInfo["PROTOCOL"] ==  "file"
        || defaultUrlInfo["PROTOCOL"] != "file"
         || obj.isEmpty() /* no launchParams, { }, and this should be check with object().isEmpty()*/
         || obj.value("contentTarget").isUndefined()
         || (m_appDesc && !m_appDesc->handlesDeeplinking())) {
        LOG_INFO(MSGID_WEBPAGE_RELAUNCH, 2, PMLOGKS("APP_ID", qPrintable(appId())), PMLOGKFV("PID", "%d", getWebProcessPID()),
            "%s; NOT enough deeplinking condition; return false", __func__);
        return false;
    }
#endif

    // Do deeplinking relaunch
    setLaunchParams(launchParams);
    return doDeeplinking(launchParams);
}

bool WebPageBase::doDeeplinking(const std::string& launchParams)
{
#if 0
    QJsonObject obj = QJsonDocument::fromJson(launchParams.toUtf8()).object();
#else
    QByteArray qBA(launchParams.c_str(), launchParams.length());
    QJsonObject obj = QJsonDocument::fromJson(qBA).object();
#endif
    if (obj.isEmpty() || obj.value("contentTarget").isUndefined())
        return false;

    std::string handledBy = obj.value("handledBy").isUndefined() ? "default" : obj.value("handledBy").toString().toStdString();
    if (handledBy == "platform") {
        std::string targetUrl = obj.value("contentTarget").toString().toStdString();
        LOG_INFO(MSGID_DEEPLINKING, 3, PMLOGKS("APP_ID", qPrintable(appId())), PMLOGKFV("PID", "%d", getWebProcessPID()),
            PMLOGKS("handledBy", handledBy.c_str()),
            "%s; load target URL:%s", __func__, targetUrl.c_str());
        // load the target URL directly
        loadUrl(targetUrl);
        return true;
    } else if(handledBy == "app") {
        // If "handledBy" == "app" return false
        // then it will be handled just like common relaunch case, checking progress
        return false;
    } else {
        // handledBy == "default" or "other values"
        LOG_INFO(MSGID_DEEPLINKING, 3, PMLOGKS("APP_ID", qPrintable(appId())), PMLOGKFV("PID", "%d", getWebProcessPID()),
            PMLOGKS("handledBy", handledBy.c_str()), "%s; loadDefaultUrl", __func__);
        loadDefaultUrl();
        return true;
    }
}

void WebPageBase::sendRelaunchEvent()
{
    setVisible(true);
    LOG_INFO(MSGID_SEND_RELAUNCHEVENT, 2, PMLOGKS("APP_ID", qPrintable(appId())), PMLOGKFV("PID", "%d", getWebProcessPID()), "");
    // Send the relaunch event on the next tick after javascript is loaded
    // This is a workaround for a problem where WebKit can't free the page
    // if we don't use a timeout here.

    std::stringstream ss;
    ss << "setTimeout(function () {" << std::endl;
    ss << "    console.log('[WAM] fires webOSRelaunch event');" << std::endl;
    ss << "    var launchEvent=new CustomEvent('webOSRelaunch', { detail: " <<
        (launchParams().empty() ? "{}" : launchParams()) << " });" << std::endl;
    ss << "    document.dispatchEvent(launchEvent);" << std::endl;
    ss << "}, 1);";

    evaluateJavaScript(ss.str());
}

void WebPageBase::urlChangedSlot()
{
    Q_EMIT webPageUrlChanged();
}

void WebPageBase::handleLoadStarted()
{
    m_suspendAtLoad = true;
}

void WebPageBase::handleLoadFinished()
{
    LOG_INFO(MSGID_WEBPAGE_LOAD_FINISHED, 2, PMLOGKS("APP_ID", qPrintable(appId())), PMLOGKFV("PID", "%d", getWebProcessPID()), "m_suspendAtLoad : %s", m_suspendAtLoad ? "true; suspend in this time" : "false");
    if (appId() == WebAppManager::instance()->getContainerAppId())
        WebAppManager::instance()->setContainerAppLaunched(true);

    Q_EMIT webPageLoadFinished();

    // if there was an attempt made to suspend while this page was loading, then
    // we flag m_suspendAtLoad = true, and suspend it after it is loaded. This is
    // to prevent application load from failing.
    if(m_suspendAtLoad) {
        suspendWebPagePaintingAndJSExecution();
    }
    updateIsLoadErrorPageFinish();
}

void WebPageBase::handleLoadFailed(int errorCode)
{
    LOG_INFO(MSGID_WEBPAGE_LOAD_FAILED, 2, PMLOGKS("APP_ID", qPrintable(appId())), PMLOGKFV("PID", "%d", getWebProcessPID()), "");

    // errorCode 204 specifically states that the web browser not relocate
    // http://www.w3.org/Protocols/rfc2616/rfc2616-sec10.html
    // we can't handle unknown protcol like mailto.
    // Client want to not show error page with unknown protocol like chrome.
    if (!m_isPreload && errorCode != 204 && errorCode != 301)
        loadErrorPage(errorCode);
}

void WebPageBase::cleanResourcesFinished()
{
    WebAppManager::instance()->postRunningAppList();
    if (m_cleaningResources) {
        WebAppManager::instance()->removeWebAppFromWebProcessInfoMap(appId());
        delete this;
    }
}

void WebPageBase::handleForceDeleteWebPage()
{
    delete this;
}

bool WebPageBase::getSystemLanguage(std::string &value)
{
    return WebAppManager::instance()->getSystemLanguage(value);
}

bool WebPageBase::getDeviceInfo(std::string name, std::string &value)
{
    return WebAppManager::instance()->getDeviceInfo(name, value);
}

int WebPageBase::currentUiWidth()
{
    return WebAppManager::instance()->currentUiWidth();
}

int WebPageBase::currentUiHeight()
{
    return WebAppManager::instance()->currentUiHeight();
}

WebProcessManager* WebPageBase::getWebProcessManager()
{
    return WebAppManager::instance()->getWebProcessManager();
}

WebAppManagerConfig* WebPageBase::getWebAppManagerConfig()
{
    return WebAppManager::instance()->config();
}

bool WebPageBase::processCrashed()
{
    return WebAppManager::instance()->processCrashed(appId());
}

int WebPageBase::suspendDelay()
{
    return WebAppManager::instance()->getSuspendDelay();
}

std::string WebPageBase::telluriumNubPath()
{
    return getWebAppManagerConfig()->getTelluriumNubPath();
}

void WebPageBase::doLoadSlot()
{
    loadDefaultUrl();
}

bool WebPageBase::hasLoadErrorPolicy(bool isHttpResponseError, int errorCode)
{
    if (!m_loadErrorPolicy.compare("event")) {

        std::stringstream ss;
        ss << "{" << std::endl;
        ss << "    console.log('[WAM3] create webOSLoadError event');" << std::endl;
        ss << "    var launchEvent=new CustomEvent('webOSLoadError', { detail : { genericError : " <<
            (isHttpResponseError?"false":"true") << ", errorCode : " <<
            errorCode << "}});" << std::endl;
        ss << "    document.dispatchEvent(launchEvent);" << std::endl;
        ss << "}";

        evaluateJavaScript(ss.str());

        //App has load error policy, do not show platform load error page
        return true;
    }
    return false;
}

void WebPageBase::applyPolicyForUrlResponse(bool isMainFrame, const std::string& url, int status_code)
{
#if 0
    QUrl qUrl(url);
    static const int s_httpErrorStatusCode = 400;
    if (qUrl.scheme() != "file" &&  status_code >= s_httpErrorStatusCode) {
        if(!hasLoadErrorPolicy(true, status_code) && isMainFrame) {
            // If app does not have policy for load error and
            // this error response is from main frame document
            // then before open server error page, reset the body's background color to white
            setBackgroundColorOfBody(QStringLiteral("white"));
        }
    }
#else
    std::string qUrl = url;
    static const int s_httpErrorStatusCode = 400;
    std::unordered_map<std::string, std::string> urlInfo;
    WamUtils::parseURL(qUrl, urlInfo);
    if (urlInfo["PROTOCOL"] != "file" &&  status_code >= s_httpErrorStatusCode) {
        if(!hasLoadErrorPolicy(true, status_code) && isMainFrame) {
            // If app does not have policy for load error and
            // this error response is from main frame document
            // then before open server error page, reset the body's background color to white
            setBackgroundColorOfBody("white");
        }
    }
#endif
}

void WebPageBase::postRunningAppList()
{
    WebAppManager::instance()->postRunningAppList();
}

void WebPageBase::postWebProcessCreated(uint32_t pid)
{
    WebAppManager::instance()->postWebProcessCreated(m_appId, pid);
}

void WebPageBase::setBackgroundColorOfBody(const std::string& color)
{
    // for error page only, set default background color to white by executing javascript
    std::stringstream ss;
    ss << "(function() {" << std::endl;
    ss << "    if(document.readyState === 'complete' || document.readyState === 'interactive') { " << std::endl;
    ss << "       if(document.body.style.backgroundColor)" << std::endl;
    ss << "           console.log('[Server Error] Already set document.body.style.backgroundColor');" << std::endl;
    ss << "       else {" << std::endl;
    ss << "           console.log('[Server Error] set background Color of body to " << color << "');" << std::endl;
    ss << "           document.body.style.backgroundColor = '" << color << "';" << std::endl;
    ss << "       }" << std::endl;
    ss << "     } else {" << std::endl;
    ss << "        document.addEventListener('DOMContentLoaded', function() {" << std::endl;
    ss << "           if(document.body.style.backgroundColor)" << std::endl;
    ss << "               console.log('[Server Error] Already set document.body.style.backgroundColor');" << std::endl;
    ss << "           else {" << std::endl;
    ss << "               console.log('[Server Error] set background Color of body to " << color << "');" << std::endl;
    ss << "               document.body.style.backgroundColor = '" << color << "';" << std::endl;
    ss << "           }" << std::endl;
    ss << "        });" << std::endl;
    ss << "    }" << std::endl;
    ss << "})();";

    evaluateJavaScript(ss.str());
}

std::string WebPageBase::defaultFont()
{
    std::string defaultFont = "LG Display-Regular";
    std::string language;
    std::string country;
    getSystemLanguage(language);
    getDeviceInfo("LocalCountry", country);

    // for the model
    if(country == "JPN")
        defaultFont = "LG Display_JP";
    else if(country == "HKG")
        defaultFont = "LG Display GP4_HK";
    // for the locale(language)
    else if(language == "ur-IN")
        defaultFont = "LG Display_Urdu";

    LOG_DEBUG("[%s] country : [%s], language : [%s], default font : [%s]", qPrintable(appId()), qPrintable(country), qPrintable(language), qPrintable(defaultFont));
    return defaultFont;
}

void WebPageBase::updateIsLoadErrorPageFinish()
{
    // ex)
    // Target error page URL : file:///usr/share/localization/webappmanager2/resources/ko/html/loaderror.html?errorCode=65&webkitErrorCode=65
    // WAM error page : file:///usr/share/localization/webappmanager2/loaderror.html
#if 0
    m_isLoadErrorPageFinish = false;

    if (!url().isLocalFile()) return;

    std::string urlString = url().toString().toStdString();
    std::string urlFileName = url().fileName().toStdString();
    std::string errorPageFileName = QUrl(getWebAppManagerConfig()->getErrorPageUrl()).fileName().toStdString();
    std::string errorPageDirPath = getWebAppManagerConfig()->getErrorPageUrl().remove(errorPageFileName);
    if (urlString.startsWith(errorPageDirPath) && !urlFileName.compare(errorPageFileName)) {
        LOG_DEBUG("[%s] This is WAM ErrorPage; URL: %s ", qPrintable(appId()), qPrintable(urlString));
        m_isLoadErrorPageFinish = true;
    }
#endif
}

#define URL_SIZE_LIMIT 768
std::string WebPageBase::truncateURL(const std::string& url)
{
#if 0
    if(url.size() < URL_SIZE_LIMIT)
        return url;
    std::string res = std::string(url);
    return res.replace(URL_SIZE_LIMIT / 2, url.size() - URL_SIZE_LIMIT, std::stringLiteral(" ... "));
#else
    return "";
#endif
}


void WebPageBase::setCustomUserScript()
{
    // 1. check app folder has userScripts
    // 2. check userscript.js there is, appfolder/webOSUserScripts/*.js
    namespace fs = std::experimental::filesystem;
    fs::path userScriptDir(m_appDesc->folderPath());

    std::string userScriptFilePath;
    if (fs::exists(userScriptDir)) {
        userScriptFilePath = userScriptDir.append(getWebAppManagerConfig()->getUserScriptPath()).string();
    }

    fs::perms perm = fs::status(userScriptFilePath).permissions();
    bool isReadOnly = (perm & fs::perms::owner_read) != fs::perms::none;
    if(!isReadOnly)
        return;

    LOG_INFO(MSGID_WAM_DEBUG, 2, PMLOGKS("APP_ID", qPrintable(appId())), PMLOGKFV("PID", "%d", getWebProcessPID()), "User Scripts exists : %s", qPrintable(userScriptFilePath));
#if 0
    // TODO : check it while porting replacement for QUrl
    addUserScriptUrl(QUrl::fromLocalFile(userScriptFilePath));
#else
    addUserScriptUrl("file:" + userScriptFilePath);
#endif
}

void WebPageBase::addObserver(WebPageObserver* observer)
{
    m_observers.addObserver(observer);
}

void WebPageBase::removeObserver(WebPageObserver* observer)
{
    m_observers.removeObserver(observer);
}

bool WebPageBase::isAccessibilityEnabled() const
{
     return WebAppManager::instance()->isAccessibilityEnabled();
}
