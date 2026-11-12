// vstparam_host - VST3 wrapper host: takes any VST3 file/bundle and lists all its parameters.
// Built against the Steinberg VST3 SDK (https://github.com/steinbergmedia/vst3sdk).

#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/plugprovider.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/pluginterfacesupport.h"
#include "public.sdk/source/vst/utility/stringconvert.h"
#include "pluginterfaces/vst/ivsthostapplication.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/base/funknown.h"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace Steinberg;
using namespace Vst;

class HostComponentHandler final : public IComponentHandler
{
public:
    HostComponentHandler () { FUNKNOWN_CTOR; }
    virtual ~HostComponentHandler () noexcept { FUNKNOWN_DTOR }

    tresult PLUGIN_API beginEdit (ParamID) SMTG_OVERRIDE { return kResultTrue; }
    tresult PLUGIN_API performEdit (ParamID, ParamValue) SMTG_OVERRIDE { return kResultTrue; }
    tresult PLUGIN_API endEdit (ParamID) SMTG_OVERRIDE { return kResultTrue; }
    tresult PLUGIN_API restartComponent (int32) SMTG_OVERRIDE { return kResultTrue; }

    DECLARE_FUNKNOWN_METHODS
    static const FUID iid;
};

IMPLEMENT_FUNKNOWN_METHODS (HostComponentHandler, IComponentHandler, IComponentHandler::iid);

static std::string toUtf8 (const String128& str)
{
    return StringConvert::convert (str);
}

static std::string trim (const std::string& s)
{
    size_t start = s.find_first_not_of (" \t\r\n");
    if (start == std::string::npos)
        return "";
    size_t end = s.find_last_not_of (" \t\r\n");
    return s.substr (start, end - start + 1);
}

int main (int argc, char* argv[])
{
    if (argc < 2)
    {
        std::cerr << "usage: vstparam_host <path-to-vst3-or-bundle> [--set <id> <norm> ...]"
                  << " [--save <file>] [--load <file>]" << std::endl;
        return 2;
    }

    const std::string path (argv[1]);
    bool doSave = false;
    bool doLoad = false;
    std::string savePath;
    std::string loadPath;
    std::vector<std::pair<std::string, double>> setPairs;
    if (argc > 2)
    {
        int i = 2;
        while (i < argc)
        {
            std::string mode (argv[i]);
            if (mode == "--set" && i + 2 < argc)
            {
                setPairs.emplace_back (argv[i + 1], std::atof (argv[i + 2]));
                i += 3;
            }
            else if (mode == "--save" && i + 1 < argc)
            {
                doSave = true;
                savePath = argv[i + 1];
                i += 2;
            }
            else if (mode == "--load" && i + 1 < argc)
            {
                doLoad = true;
                loadPath = argv[i + 1];
                i += 2;
            }
            else
            {
                std::cerr << "vstparam_host: unknown or incomplete argument: "
                          << mode << std::endl;
                return 2;
            }
        }
    }

    // Host context: provides the mandatory IHostApplication to the plug-in.
    Vst::HostApplication hostApp;
    Vst::PluginContextFactory::instance ().setPluginContext (&hostApp);

    std::string error;
    auto module = VST3::Hosting::Module::create (path, error);
    if (!module)
    {
        std::cerr << "vstparam_host: failed to load module '" << path << "': " << error
                  << std::endl;
        return 1;
    }

    auto factory = module->getFactory ();
    auto classInfos = factory.classInfos ();

    for (auto& classInfo : classInfos)
    {
        if (classInfo.category () != kVstAudioEffectClass)
            continue;

        auto plugProvider = owned (new PlugProvider (factory, classInfo, true));
        if (!plugProvider->initialize ())
        {
            std::cerr << "vstparam_host: failed to initialize plug-in '" << classInfo.name ()
                      << "' for '" << path << "'" << std::endl;
            return 1;
        }

        IEditController* controller = plugProvider->getController ();
        if (!controller)
        {
            std::cerr << "vstparam_host: plug-in '" << classInfo.name() << "' has no controller"
                      << std::endl;
            return 1;
        }

        std::cout << "plug-in: " << classInfo.name () << " | vendor: " << classInfo.vendor ()
                  << " | version: " << classInfo.version () << std::endl;

        int32 parameterCount = controller->getParameterCount ();

        if (!setPairs.empty ())
        {
            for (const auto& sp : setPairs)
            {
                long long targetId = std::strtoll (sp.first.c_str (), nullptr, 0);
                bool found = false;
                for (int32 i = 0; i < parameterCount; ++i)
                {
                    Vst::ParameterInfo info;
                    if (controller->getParameterInfo (i, info) != kResultTrue)
                        continue;
                    if ((long long) info.id != targetId)
                        continue;

                    Vst::ParamValue value = static_cast<Vst::ParamValue> (sp.second);
                    tresult r = controller->setParamNormalized (info.id, value);
                    Vst::ParamValue readback = controller->getParamNormalized (info.id);
                    std::cout << "[ " << i << "] id=0x" << std::hex << info.id << std::dec
                              << " " << toUtf8 (info.title) << " set -> value=" << sp.second
                              << " readback=" << readback << " ret=" << r << std::endl;
                    found = true;
                }
                if (!found)
                {
                    std::cerr << "vstparam_host: parameter id " << sp.first
                              << " not found" << std::endl;
                }
            }
        }

        if (doSave)
        {
            std::ofstream out (savePath.c_str ());
            if (!out.is_open ())
            {
                std::cerr << "vstparam_host: cannot open snapshot file '" << savePath << "'"
                          << std::endl;
                controller->release ();
                return 1;
            }
            out << "# VST3 parameter preset snapshot" << std::endl;
            out << "# plugin: " << classInfo.name () << std::endl;
            out << "# vendor: " << classInfo.vendor () << std::endl;
            out << "# version: " << classInfo.version () << std::endl;
            out << "# format: <paramID decimal> <normalizedValue 0..1>" << std::endl;
            int savedCount = 0;
            for (int32 i = 0; i < parameterCount; ++i)
            {
                Vst::ParameterInfo info;
                if (controller->getParameterInfo (i, info) != kResultTrue)
                    continue;
                Vst::ParamValue value = controller->getParamNormalized (info.id);
                out << (long long) info.id << " " << value << std::endl;
                ++savedCount;
            }
            out.close ();
            std::cout << "saved snapshot of " << savedCount << " parameters to " << savePath
                      << std::endl;
            controller->release ();
            return 0;
        }

        if (doLoad)
        {
            std::ifstream in (loadPath.c_str ());
            if (!in.is_open ())
            {
                std::cerr << "vstparam_host: cannot open snapshot file '" << loadPath << "'"
                          << std::endl;
                controller->release ();
                return 1;
            }
            std::string line;
            std::string snap_plugin, snap_vendor, snap_version;
            // Read header info (plugin/vendor/version lines) first
            while (std::getline (in, line))
            {
                if (line.empty () || line[0] != '#')
                    break;
                if (line.find ("# plugin:") == 0)
                    snap_plugin = trim (line.substr (9));
                else if (line.find ("# vendor:") == 0)
                    snap_vendor = trim (line.substr (9));
                else if (line.find ("# version:") == 0)
                    snap_version = trim (line.substr (10));
            }
            if (!snap_plugin.empty ())
            {
                std::cout << "Snapshot: " << snap_plugin
                          << " | " << snap_vendor
                          << " | " << snap_version << std::endl;
            }
            // Re-process data lines from current stream position
            int loadedCount = 0;
            while (std::getline (in, line))
            {
                if (line.empty () || line[0] == '#')
                    continue;
                std::istringstream ls (line);
                long long id = 0;
                double value = 0.0;
                if (!(ls >> id >> value))
                    continue;
                for (int32 i = 0; i < parameterCount; ++i)
                {
                    Vst::ParameterInfo info;
                    if (controller->getParameterInfo (i, info) != kResultTrue)
                        continue;
                    if ((long long) info.id != id)
                        continue;
                    controller->setParamNormalized (info.id, (Vst::ParamValue) value);
                    ++loadedCount;
                    break;
                }
            }
            std::cout << "loaded snapshot of " << loadedCount << " parameters from " << loadPath
                      << std::endl;
        }

        for (int32 i = 0; i < parameterCount; ++i)
        {
            Vst::ParameterInfo info;
            if (controller->getParameterInfo (i, info) != kResultTrue)
                continue;

            Vst::ParamValue value = controller->getParamNormalized (info.id);
            Vst::ParamValue def = info.defaultNormalizedValue;

            std::string title = toUtf8 (info.title);
            std::string units = toUtf8 (info.units);

            std::cout << "[ " << i << "] id=0x" << std::hex << info.id << std::dec
                      << " " << title << " def=" << info.defaultNormalizedValue
                      << " value=" << value << " units=" << units
                      << " flags=" << info.flags << std::endl;
        }

        controller->release ();
    }

    return 0;
}
