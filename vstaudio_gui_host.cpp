// vstaudio_gui_host.cpp - VST3 audio host with GUI + keyboard input
// Built against the Steinberg VST3 SDK (https://github.com/steinbergmedia/vst3sdk)
//
// Usage:
//   vstaudio_gui_host <path-to-vst3>                    // load plugin, enter audio+GUI mode
//   vstaudio_gui_host <path-to-vst3> --load <snapshot>  // load preset then enter audio mode
//   vstaudio_gui_host <path-to-vst3> --list             // list parameters only
//
// Keyboard (tracker-style, QWERTY):
//   Row 1: Z X C V B N M , . /     -> C D E F G A B C D E  (octave 4)
//   Row 2:   S D G H J             -> C# D# F# G# A#       (octave 4)
//   Row 3: Q W E R T Y U I O P     -> C D E F G A B C D E  (octave 5)
//   Row 4:     2 3     5 6 7     9 0   -> C# D# F# G# A#  C# D#  (octave 5)
//   Space: sustain / release all notes
//   - / +: decrease/increase volume (param named "Volume")
//   [ / ]: decrease/increase cutoff (param named "Cutoff")
//   q:   quit

#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/plugprovider.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/pluginterfacesupport.h"
#include "public.sdk/source/vst/hosting/eventlist.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"
#include "public.sdk/source/vst/utility/stringconvert.h"
#include "pluginterfaces/vst/ivsthostapplication.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/vstspeaker.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/base/funknown.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <alsa/asoundlib.h>
#include <termios.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <vector>
#include <map>
#include <cmath>
#include <cstring>
#include <iostream>
#include <string>
#include <cstdlib>
#include <algorithm>
#include <fstream>
#include <sstream>

using namespace Steinberg;
using namespace Vst;

//==============================================================================
// Minimal IComponentHandler
//==============================================================================
class HostComponentHandler final : public IComponentHandler
{
public:
    HostComponentHandler () { FUNKNOWN_CTOR; }
    virtual ~HostComponentHandler () noexcept { FUNKNOWN_DTOR; }

    tresult PLUGIN_API beginEdit (ParamID) SMTG_OVERRIDE { return kResultTrue; }
    tresult PLUGIN_API performEdit (ParamID, ParamValue) SMTG_OVERRIDE { return kResultTrue; }
    tresult PLUGIN_API endEdit (ParamID) SMTG_OVERRIDE { return kResultTrue; }
    tresult PLUGIN_API restartComponent (int32) SMTG_OVERRIDE { return kResultTrue; }

    DECLARE_FUNKNOWN_METHODS
    static const FUID iid;
};

IMPLEMENT_FUNKNOWN_METHODS (HostComponentHandler, IComponentHandler, IComponentHandler::iid);
const FUID HostComponentHandler::iid = IComponentHandler::iid;

//==============================================================================
// EventList for sending MIDI events to the plugin
//==============================================================================
class HostEventList final : public IEventList
{
public:
    HostEventList () { FUNKNOWN_CTOR; }
    virtual ~HostEventList () noexcept { FUNKNOWN_DTOR; }

    int32 PLUGIN_API getEventCount () SMTG_OVERRIDE { return (int32)events.size (); }
    tresult PLUGIN_API getEvent (int32 index, Event& e) SMTG_OVERRIDE
    {
        if (index < 0 || index >= (int32)events.size ())
            return kInvalidArgument;
        e = events[index];
        return kResultTrue;
    }
    tresult PLUGIN_API addEvent (Event& e) SMTG_OVERRIDE
    {
        events.push_back (e);
        return kResultTrue;
    }
    void clear () { events.clear (); }

    DECLARE_FUNKNOWN_METHODS
    static const FUID iid;

private:
    std::vector<Event> events;
};

IMPLEMENT_FUNKNOWN_METHODS (HostEventList, IEventList, IEventList::iid);
const FUID HostEventList::iid = IEventList::iid;

//==============================================================================
// Signal handler for debugging crashes
//==============================================================================
static void segv_handler (int sig)
{
    std::cerr << "\n*** SEGMENTATION FAULT (signal " << sig << ") ***" << std::endl;
    _exit (1);
}

static void install_signal_handlers ()
{
    struct sigaction sa;
    memset (&sa, 0, sizeof (sa));
    sa.sa_handler = segv_handler;
    sigaction (SIGSEGV, &sa, nullptr);
    sigaction (SIGABRT, &sa, nullptr);
    sigaction (SIGBUS, &sa, nullptr);
}

//==============================================================================
// X11 Window management with plugin GUI embedding
//==============================================================================
class X11Window
{
public:
    X11Window () : display (nullptr), window (0), screen (0), gc (nullptr), atomWmDelete (0) {}

    bool open (int width, int height, const char* title)
    {
        // X11 is not thread-safe by default; the plugin's internal JUCE event
        // loop thread will also touch the X display while we poll events here.
        XInitThreads ();
        display = XOpenDisplay (nullptr);
        if (!display)
        {
            std::cerr << "X11: cannot open display" << std::endl;
            return false;
        }

        screen = DefaultScreen (display);
        Window root = DefaultRootWindow (display);

        XSetWindowAttributes attr;
        attr.background_pixel = BlackPixel (display, screen);
        attr.border_pixel = WhitePixel (display, screen);
        attr.event_mask = ExposureMask | StructureNotifyMask | KeyPressMask | KeyReleaseMask | FocusChangeMask;

        window = XCreateWindow (display, root, 0, 0, width, height, 0,
                                DefaultDepth (display, screen), InputOutput,
                                DefaultVisual (display, screen),
                                CWBackPixel | CWBorderPixel | CWEventMask, &attr);

        if (!window)
        {
            std::cerr << "X11: cannot create window" << std::endl;
            XCloseDisplay (display);
            display = nullptr;
            return false;
        }

        XStoreName (display, window, title);
        XMapWindow (display, window);

        atomWmDelete = XInternAtom (display, "WM_DELETE_WINDOW", False);
        XSetWMProtocols (display, window, &atomWmDelete, 1);

        gc = XCreateGC (display, window, 0, nullptr);
        XSetForeground (display, gc, WhitePixel (display, screen));

        XFlush (display);
        return true;
    }

    void embedWindow (Window pluginWindow, int x, int y, int w, int h)
    {
        if (!display || !window || !pluginWindow)
            return;

        XReparentWindow (display, pluginWindow, window, x, y);
        XMapWindow (display, pluginWindow);
        XResizeWindow (display, pluginWindow, w, h);
        XFlush (display);
    }

    void resize (int width, int height)
    {
        if (display && window)
        {
            XResizeWindow (display, window, width, height);
            XFlush (display);
        }
    }

    void close ()
    {
        if (gc)
        {
            XFreeGC (display, gc);
            gc = nullptr;
        }
        if (window)
        {
            XDestroyWindow (display, window);
            window = 0;
        }
        if (display)
        {
            XCloseDisplay (display);
            display = nullptr;
        }
    }

    // Check for X11 events, return false if window was closed
    bool pollEvents (bool& shouldQuit)
    {
        shouldQuit = false;
        if (!display)
            return false;

        while (XPending (display))
        {
            XEvent ev;
            XNextEvent (display, &ev);

            switch (ev.type)
            {
            case ClientMessage:
                if ((Atom)ev.xclient.data.l[0] == atomWmDelete)
                    shouldQuit = true;
                break;
            case DestroyNotify:
                shouldQuit = true;
                break;
            case Expose:
                // Redraw if needed
                break;
            case ConfigureNotify:
                // Window resized
                break;
            }
        }
        return true;
    }

    Display* display;
    Window window;
    int screen;
    GC gc;
    Atom atomWmDelete;
};

//==============================================================================
// VST3 GUI frame callback. Plug-ins use this when they request a resize.
//==============================================================================
class HostPlugFrame final : public IPlugFrame
{
public:
    explicit HostPlugFrame (X11Window* window) : window_ (window) { FUNKNOWN_CTOR; }
    virtual ~HostPlugFrame () noexcept { FUNKNOWN_DTOR; }

    tresult PLUGIN_API resizeView (IPlugView* view, ViewRect* newSize) SMTG_OVERRIDE
    {
        if (!view || !newSize)
            return kInvalidArgument;
        if (window_)
            window_->resize (newSize->getWidth (), newSize->getHeight ());
        return view->onSize (newSize);
    }

    DECLARE_FUNKNOWN_METHODS
    static const FUID iid;

private:
    X11Window* window_;
};

IMPLEMENT_FUNKNOWN_METHODS (HostPlugFrame, IPlugFrame, IPlugFrame::iid);
const FUID HostPlugFrame::iid = IPlugFrame::iid;

//==============================================================================
// Terminal handling (raw mode, non-blocking input)
//==============================================================================
class Terminal
{
public:
    Terminal () : fd_ (STDIN_FILENO), raw_ (false) {}

    bool enable ()
    {
        if (tcgetattr (fd_, &orig_) != 0)
            return false;
        termios raw = orig_;
        raw.c_lflag &= ~(ECHO | ICANON);
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
        if (tcsetattr (fd_, TCSANOW, &raw) != 0)
            return false;
        raw_ = true;

        int flags = fcntl (fd_, F_GETFL, 0);
        fcntl (fd_, F_SETFL, flags | O_NONBLOCK);
        return true;
    }

    void disable ()
    {
        if (raw_)
        {
            tcsetattr (fd_, TCSANOW, &orig_);
            raw_ = false;
        }
    }

    ~Terminal () { disable (); }

    bool hasKey ()
    {
        struct pollfd pfd;
        pfd.fd = fd_;
        pfd.events = POLLIN;
        int r = poll (&pfd, 1, 0);
        return r > 0;
    }

    int getKey ()
    {
        unsigned char c;
        int r = read (fd_, &c, 1);
        if (r <= 0)
            return -1;
        return (int)c;
    }

private:
    int fd_;
    struct termios orig_;
    bool raw_;
};

//==============================================================================
// ALSA audio output
//==============================================================================
class AudioOut
{
public:
    AudioOut () : pcm_ (nullptr), sampleRate_ (44100), channels_ (2) {}

    bool open (unsigned rate, unsigned channels)
    {
        sampleRate_ = rate;
        channels_ = channels;

        int err = snd_pcm_open (&pcm_, "default", SND_PCM_STREAM_PLAYBACK, 0);
        if (err < 0)
        {
            std::cerr << "ALSA: cannot open device: " << snd_strerror (err) << std::endl;
            return false;
        }

        snd_pcm_hw_params_t* params;
        snd_pcm_hw_params_alloca (&params);
        snd_pcm_hw_params_any (pcm_, params);
        snd_pcm_hw_params_set_access (pcm_, params, SND_PCM_ACCESS_RW_INTERLEAVED);
        snd_pcm_hw_params_set_format (pcm_, params, SND_PCM_FORMAT_S16_LE);
        snd_pcm_hw_params_set_rate (pcm_, params, rate, 0);
        snd_pcm_hw_params_set_channels (pcm_, params, channels);

        err = snd_pcm_hw_params (pcm_, params);
        if (err < 0)
        {
            std::cerr << "ALSA: cannot set hardware params: " << snd_strerror (err) << std::endl;
            snd_pcm_close (pcm_);
            pcm_ = nullptr;
            return false;
        }

        snd_pcm_hw_params_get_period_size (params, &periodSize_, 0);
        std::cerr << "ALSA: period size = " << periodSize_ << " frames" << std::endl;

        err = snd_pcm_prepare (pcm_);
        if (err < 0)
        {
            std::cerr << "ALSA: cannot prepare: " << snd_strerror (err) << std::endl;
            snd_pcm_close (pcm_);
            pcm_ = nullptr;
            return false;
        }

        return true;
    }

    void writeFloat (float* buffer, int frames)
    {
        if (!pcm_)
            return;

        static thread_local std::vector<short> buf;
        buf.resize (frames * channels_);

        for (int i = 0; i < frames * channels_; ++i)
        {
            float v = buffer[i];
            if (v > 1.0f) v = 1.0f;
            if (v < -1.0f) v = -1.0f;
            buf[i] = (short)(v * 32767.0f);
        }

        int err = snd_pcm_writei (pcm_, buf.data (), frames);
        if (err == -EPIPE)
        {
            snd_pcm_prepare (pcm_);
        }
        else if (err < 0)
        {
            std::cerr << "ALSA write error: " << snd_strerror (err) << std::endl;
        }
    }

    void close ()
    {
        if (pcm_)
        {
            snd_pcm_drain (pcm_);
            snd_pcm_close (pcm_);
            pcm_ = nullptr;
        }
    }

    ~AudioOut () { close (); }

    snd_pcm_t* pcm_;
    unsigned sampleRate_;
    unsigned channels_;
    snd_pcm_uframes_t periodSize_;
};

//==============================================================================
struct ActiveNote
{
    uint16 noteId;
    uint16 pitch;
};

//==============================================================================
struct KeyMapEntry
{
    int key;
    uint16 note;
};

static const KeyMapEntry keyMap[] =
{
    {'z', 60}, {'x', 62}, {'c', 64}, {'v', 65}, {'b', 67}, {'n', 69}, {'m', 71},
    {',', 72}, {'.', 74}, {'/', 76},
    {'s', 61}, {'d', 63}, {'g', 66}, {'h', 68}, {'j', 70},
    {'q', 72}, {'w', 74}, {'e', 76}, {'r', 77}, {'t', 79}, {'y', 81}, {'u', 83},
    {'i', 84}, {'o', 86}, {'p', 88},
    {'2', 73}, {'3', 75}, {'5', 80}, {'6', 82}, {'7', 84},
    {'9', 85}, {'0', 87},
};

static const int keyMapSize = sizeof (keyMap) / sizeof (keyMap[0]);

static uint16 keyToNote (int key)
{
    for (int i = 0; i < keyMapSize; ++i)
    {
        if (keyMap[i].key == key)
            return keyMap[i].note;
    }
    return 255;
}

//==============================================================================
static int32 findParamByName (IEditController* controller, const char* name)
{
    if (!controller)
        return -1;
    int32 count = controller->getParameterCount ();
    std::string nameLower = name;
    std::transform (nameLower.begin (), nameLower.end (), nameLower.begin (), ::tolower);
    for (int32 i = 0; i < count; ++i)
    {
        ParameterInfo info;
        if (controller->getParameterInfo (i, info) != kResultTrue)
            continue;
        std::string title = StringConvert::convert (info.title);
        std::string titleLower = title;
        std::transform (titleLower.begin (), titleLower.end (), titleLower.begin (), ::tolower);
        if (titleLower.find (nameLower) != std::string::npos)
            return i;
    }
    return -1;
}

//==============================================================================
int main (int argc, char* argv[])
{
    if (argc < 2)
    {
        std::cerr << "usage: vstaudio_gui_host <path-to-vst3> [--load <snapshot>] [--list]" << std::endl;
        std::cerr << "  --load <file>   load preset snapshot before entering audio mode" << std::endl;
        std::cerr << "  --list          list parameters and exit" << std::endl;
        return 2;
    }

    std::string path = argv[1];
    std::string loadPath;
    bool listOnly = false;
    bool demoMode = false;

    for (int i = 2; i < argc; ++i)
    {
        std::string arg = argv[i];
        if (arg == "--load" && i + 1 < argc)
        {
            loadPath = argv[++i];
        }
        else if (arg == "--list")
        {
            listOnly = true;
        }
        else if (arg == "--demo")
        {
            demoMode = true;
        }
    }

    // Host context
    Vst::HostApplication hostApp;
    Vst::PluginContextFactory::instance ().setPluginContext (&hostApp);

    install_signal_handlers ();

    // Load module
    std::string error;
    auto module = VST3::Hosting::Module::create (path, error);
    if (!module)
    {
        std::cerr << "vstaudio_gui_host: failed to load module '" << path << "': " << error << std::endl;
        return 1;
    }

    auto factory = module->getFactory ();
    auto classInfos = factory.classInfos ();

    // Find Audio Module Class
    VST3::Hosting::ClassInfo classInfo;
    bool found = false;
    for (auto& ci : classInfos)
    {
        if (ci.category () == kVstAudioEffectClass)
        {
            classInfo = ci;
            found = true;
            break;
        }
    }

    if (!found)
    {
        std::cerr << "vstaudio_gui_host: no Audio Module Class found in '" << path << "'" << std::endl;
        return 1;
    }

    std::cout << "plug-in: " << classInfo.name () << " | vendor: " << classInfo.vendor ()
              << " | version: " << classInfo.version () << std::endl;

    auto plugProvider = owned (new PlugProvider (factory, classInfo, true));
    if (!plugProvider->initialize ())
    {
        std::cerr << "vstaudio_gui_host: failed to initialize plug-in" << std::endl;
        return 1;
    }

    IComponent* component = plugProvider->getComponent ();
    IEditController* controller = plugProvider->getController ();

    if (!component)
    {
        std::cerr << "vstaudio_gui_host: plug-in has no component" << std::endl;
        return 1;
    }

    if (!controller)
    {
        std::cerr << "vstaudio_gui_host: plug-in has no controller" << std::endl;
        return 1;
    }

    // Set component handler
    auto handler = owned (new HostComponentHandler ());
    controller->setComponentHandler (handler.get ());

    // Create the editor view through the edit controller. The host must create
    // a parent X11EmbedWindowID and then call IPlugView::attached().
    IPlugView* plugView = controller->createView (ViewType::kEditor);
    bool hasPlugView = plugView != nullptr;
    bool supportsX11 = false;
    ViewRect plugRect;
    X11Window x11;
    bool hasX11 = false;
    IPtr<HostPlugFrame> plugFrame;

    if (hasPlugView)
    {
        supportsX11 = plugView->isPlatformTypeSupported (kPlatformTypeX11EmbedWindowID) == kResultTrue;
        if (supportsX11 && plugView->getSize (&plugRect) == kResultTrue)
        {
            std::cerr << "plug-in GUI: X11 supported, size=" << plugRect.getWidth ()
                      << "x" << plugRect.getHeight () << std::endl;
        }
        else
        {
            std::cerr << "plug-in GUI: X11 view unsupported or size unavailable" << std::endl;
        }
    }
    else
    {
        std::cerr << "vstaudio_gui_host: plug-in has no editor view" << std::endl;
    }

    // If --list only, print parameters and exit
    if (listOnly)
    {
        int32 paramCount = controller->getParameterCount ();
        std::cout << "parameters: " << paramCount << std::endl;
        for (int32 i = 0; i < paramCount; ++i)
        {
            ParameterInfo info;
            if (controller->getParameterInfo (i, info) != kResultTrue)
                continue;
            std::string title = StringConvert::convert (info.title);
            std::string units = StringConvert::convert (info.units);
            std::cout << "[" << i << "] id=0x" << std::hex << info.id << std::dec
                      << " " << title << " def=" << info.defaultNormalizedValue
                      << " units=" << units << " flags=" << info.flags << std::endl;
        }
        return 0;
    }

    // Load snapshot if specified. Keep the values so the processor receives
    // them as an initial IParameterChanges block during the first process call.
    std::vector<std::pair<ParamID, ParamValue>> loadedParams;
    if (!loadPath.empty ())
    {
        std::ifstream in (loadPath.c_str ());
        if (!in.is_open ())
        {
            std::cerr << "vstaudio_gui_host: cannot open snapshot '" << loadPath << "'" << std::endl;
            return 1;
        }
        std::string line;
        int loaded = 0;
        while (std::getline (in, line))
        {
            if (line.empty () || line[0] == '#')
                continue;
            std::istringstream ls (line);
            long long id = 0;
            double value = 0.0;
            if (!(ls >> id >> value))
                continue;
            for (int32 i = 0; i < controller->getParameterCount (); ++i)
            {
                ParameterInfo info;
                if (controller->getParameterInfo (i, info) != kResultTrue)
                    continue;
                if ((long long) info.id != id)
                    continue;
                controller->setParamNormalized (info.id, (Vst::ParamValue) value);
                loadedParams.emplace_back (info.id, (Vst::ParamValue) value);
                ++loaded;
                break;
            }
        }
        std::cout << "loaded snapshot of " << loaded << " parameters from " << loadPath << std::endl;
    }

    //--------------------------------------------------------------
    // Set up audio buses
    //--------------------------------------------------------------
    int32 audioOutCount = component->getBusCount (kAudio, kOutput);
    int32 eventInCount = component->getBusCount (kEvent, kInput);

    std::cerr << "audio output buses: " << audioOutCount << std::endl;
    std::cerr << "event input buses: " << eventInCount << std::endl;

    if (audioOutCount > 0)
    {
        BusInfo info;
        component->getBusInfo (kAudio, kOutput, 0, info);
        std::cerr << "output bus 0: channels=" << info.channelCount << std::endl;
        component->activateBus (kAudio, kOutput, 0, true);
    }

    bool hasEventInput = false;
    if (eventInCount > 0)
    {
        BusInfo info;
        component->getBusInfo (kEvent, kInput, 0, info);
        std::cerr << "event input bus 0: channels=" << info.channelCount << std::endl;
        component->activateBus (kEvent, kInput, 0, true);
        hasEventInput = true;
    }

    // Get IAudioProcessor for processing
    auto audioProcessor = U::cast<IAudioProcessor> (component);
    if (!audioProcessor)
    {
        std::cerr << "vstaudio_gui_host: plug-in has no IAudioProcessor" << std::endl;
        return 1;
    }

    // Explicitly select the plug-in's output arrangement before setup.
    SpeakerArrangement outputArrangement = 0;
    if (audioProcessor->getBusArrangement (kOutput, 0, outputArrangement) != kResultTrue)
        outputArrangement = SpeakerArr::kStereo;
    auto arrangementResult = audioProcessor->setBusArrangements (
        nullptr, 0, &outputArrangement, 1);
    std::cerr << "output arrangement result: " << arrangementResult << std::endl;

    // Setup processing
    ProcessSetup setup;
    setup.processMode = kRealtime;
    setup.sampleRate = 44100.0;
    setup.maxSamplesPerBlock = 512;
    setup.symbolicSampleSize = kSample32;

    if (audioProcessor->setupProcessing (setup) != kResultTrue)
    {
        std::cerr << "vstaudio_gui_host: failed to setup processing" << std::endl;
        return 1;
    }

    if (component->setActive (true) != kResultTrue)
    {
        std::cerr << "vstaudio_gui_host: failed to activate component" << std::endl;
        return 1;
    }

    std::cerr << "component activated, sampleRate=" << setup.sampleRate
              << " maxSamplesPerBlock=" << setup.maxSamplesPerBlock << std::endl;

    //--------------------------------------------------------------
    // Set up X11 window
    //--------------------------------------------------------------
    int guiWidth = plugRect.getWidth () > 0 ? plugRect.getWidth () : 400;
    int guiHeight = plugRect.getHeight () > 0 ? plugRect.getHeight () : 400;

    if (hasPlugView && supportsX11)
    {
        if (x11.open (guiWidth, guiHeight, classInfo.name ().c_str ()))
        {
            plugFrame = owned (new HostPlugFrame (&x11));
            plugView->setFrame (plugFrame.get ());
            auto attached = plugView->attached (
                reinterpret_cast<void*> (x11.window), kPlatformTypeX11EmbedWindowID);
            if (attached == kResultTrue)
            {
                hasX11 = true;
                std::cerr << "GUI window embedded" << std::endl;
            }
            else
            {
                std::cerr << "plug-in GUI: attached() failed: " << attached << std::endl;
                plugView->setFrame (nullptr);
                plugFrame = nullptr;
                x11.close ();
            }
        }
        else
        {
            std::cerr << "failed to create X11 window" << std::endl;
        }
    }

    //--------------------------------------------------------------
    // Set up terminal + ALSA
    //--------------------------------------------------------------
    Terminal term;
    bool termOk = term.enable ();
    if (!termOk)
    {
        std::cerr << "note: not a TTY, running in demo mode" << std::endl;
        demoMode = true;
    }

    AudioOut audio;
    if (!audio.open (44100, 2))
    {
        std::cerr << "failed to open ALSA audio" << std::endl;
        return 1;
    }

    unsigned periodSize = audio.periodSize_;
    unsigned sampleRate = (unsigned) audio.sampleRate_;
    unsigned channels = audio.channels_;

    // Audio buffers (float interleaved) - ZERO INITIALIZED
    std::vector<float> audioBuf (periodSize * channels, 0.0f);

    // Event list for MIDI input
    auto eventList = owned (new HostEventList ());

    // Deliver loaded snapshot values to the processor on the first block.
    ParameterChanges initialParamChanges ((int32) loadedParams.size ());
    for (const auto& change : loadedParams)
    {
        int32 queueIndex = -1;
        if (auto* queue = initialParamChanges.addParameterData (change.first, queueIndex))
        {
            int32 pointIndex = -1;
            queue->addPoint (0, change.second, pointIndex);
        }
    }
    bool initialParamsPending = !loadedParams.empty ();

    // Active notes tracking
    std::map<uint16, ActiveNote> activeNotes;
    uint16 nextNoteId = 1;

    // Sustain state
    bool sustain = false;

    std::cerr << "=== Audio host ready ===" << std::endl;
    std::cerr << "Keyboard (tracker style):" << std::endl;
    std::cerr << "  Z X C V B N M , . /  -> C D E F G A B C D E" << std::endl;
    std::cerr << "    S D G H J          -> C# D# F# G# A#" << std::endl;
    std::cerr << "  Q W E R T Y U I O P  -> C D E F G A B C D E (octave +1)" << std::endl;
    std::cerr << "    2 3     5 6 7     9 0  -> C# D# F# G# A#  C# D#" << std::endl;
    std::cerr << "  Space: sustain / release all" << std::endl;
    std::cerr << "  - / +: volume down/up" << std::endl;
    std::cerr << "  [ / ]: filter cutoff down/up" << std::endl;
    std::cerr << "  q: quit" << std::endl;
    std::cerr << "========================" << std::endl;

    // Find volume and cutoff parameters for quick tweak
    int volIdx = findParamByName (controller, "volume");
    int cutoffIdx = findParamByName (controller, "cutoff");

    // Demo mode state
    int demoStep = 0;
    int demoGateCounter = 0;
    const int demoPattern[] = { 60, 60, 60, 67, 60, 65, 60, 67, 60, 62, 60, 65, 57, 60, 62, 64 };
    const int demoPatternLen = 16;

    // Main loop
    bool running = true;
    while (running)
    {
        //--------------------------------------------------------------
        // Handle X11 events (non-blocking)
        //--------------------------------------------------------------
        if (hasX11)
        {
            bool shouldQuit = false;
            if (!x11.pollEvents (shouldQuit))
                break;
            if (shouldQuit)
                break;
        }

        //--------------------------------------------------------------
        // Handle keyboard input (non-blocking)
        //--------------------------------------------------------------
        if (termOk)
        {
            while (term.hasKey ())
            {
                int key = term.getKey ();
                if (key < 0)
                    break;

                switch (key)
                {
                case 'q':
                case 'Q':
                    running = false;
                    break;

                case ' ':
                    sustain = !sustain;
                    if (!sustain)
                    {
                        for (auto& kv : activeNotes)
                        {
                            Event ev;
                            memset (&ev, 0, sizeof (ev));
                            ev.busIndex = 0;
                            ev.sampleOffset = 0;
                            ev.ppqPosition = 0;
                            ev.type = Event::kNoteOffEvent;
                            ev.flags = Event::kIsLive;
                            ev.noteOff.pitch = kv.second.pitch;
                            ev.noteOff.velocity = 0;
                            ev.noteOff.noteId = kv.first;
                            eventList->addEvent (ev);
                        }
                        activeNotes.clear ();
                        nextNoteId = 1;
                    }
                    std::cerr << "sustain: " << (sustain ? "ON" : "OFF") << std::endl;
                    break;

                case '-':
                case '_':
                    if (volIdx >= 0)
                    {
                        ParameterInfo info;
                        if (controller->getParameterInfo (volIdx, info) == kResultTrue)
                        {
                            double v = controller->getParamNormalized (info.id);
                            v -= 0.05;
                            if (v < 0.0) v = 0.0;
                            controller->setParamNormalized (info.id, (Vst::ParamValue) v);
                            std::cerr << "volume: " << v << std::endl;
                        }
                    }
                    break;

                case '=':
                case '+':
                    if (volIdx >= 0)
                    {
                        ParameterInfo info;
                        if (controller->getParameterInfo (volIdx, info) == kResultTrue)
                        {
                            double v = controller->getParamNormalized (info.id);
                            v += 0.05;
                            if (v > 1.0) v = 1.0;
                            controller->setParamNormalized (info.id, (Vst::ParamValue) v);
                            std::cerr << "volume: " << v << std::endl;
                        }
                    }
                    break;

                case '[':
                    if (cutoffIdx >= 0)
                    {
                        ParameterInfo info2;
                        controller->getParameterInfo (cutoffIdx, info2);
                        double v2 = controller->getParamNormalized (info2.id);
                        v2 -= 0.05;
                        if (v2 < 0.0) v2 = 0.0;
                        controller->setParamNormalized (info2.id, (Vst::ParamValue) v2);
                        std::cerr << "cutoff: " << v2 << std::endl;
                    }
                    break;

                case ']':
                    if (cutoffIdx >= 0)
                    {
                        ParameterInfo info2;
                        controller->getParameterInfo (cutoffIdx, info2);
                        double v2 = controller->getParamNormalized (info2.id);
                        v2 += 0.05;
                        if (v2 > 1.0) v2 = 1.0;
                        controller->setParamNormalized (info2.id, (Vst::ParamValue) v2);
                        std::cerr << "cutoff: " << v2 << std::endl;
                    }
                    break;

                default:
                {
                    uint16 note = keyToNote (key);
                    if (note != 255 && hasEventInput)
                    {
                        Event ev;
                        memset (&ev, 0, sizeof (ev));
                        ev.busIndex = 0;
                        ev.sampleOffset = 0;
                        ev.ppqPosition = 0;
                        ev.type = Event::kNoteOnEvent;
                        ev.flags = Event::kIsLive;
                        ev.noteOn.pitch = note;
                        ev.noteOn.velocity = 100;
                        ev.noteOn.length = 0;
                        ev.noteOn.noteId = nextNoteId;
                        ev.noteOn.tuning = 0.0f;
                        eventList->addEvent (ev);

                        ActiveNote an;
                        an.noteId = nextNoteId;
                        an.pitch = note;
                        activeNotes[nextNoteId] = an;
                        nextNoteId++;

                        std::cerr << "note on: " << (int)note << " (id=" << (int)ev.noteOn.noteId << ")" << std::endl;
                    }
                    break;
                }
                }
            }
        }

        //--------------------------------------------------------------
        // Demo mode: auto-play a bassline pattern
        //--------------------------------------------------------------
        if (demoMode && hasEventInput)
        {
            demoGateCounter++;
            if (demoGateCounter >= 4)
            {
                demoGateCounter = 0;
                int note = demoPattern[demoStep];
                demoStep = (demoStep + 1) % demoPatternLen;

                Event evOn;
                memset (&evOn, 0, sizeof (evOn));
                evOn.busIndex = 0;
                evOn.sampleOffset = 0;
                evOn.ppqPosition = 0;
                evOn.type = Event::kNoteOnEvent;
                evOn.flags = Event::kIsLive;
                evOn.noteOn.pitch = note;
                evOn.noteOn.velocity = 90;
                evOn.noteOn.length = 0;
                evOn.noteOn.noteId = nextNoteId;
                evOn.noteOn.tuning = 0.0f;
                eventList->addEvent (evOn);

                ActiveNote an;
                an.noteId = nextNoteId;
                an.pitch = note;
                activeNotes[nextNoteId] = an;
                nextNoteId++;

                Event evOff;
                memset (&evOff, 0, sizeof (evOff));
                evOff.busIndex = 0;
                evOff.sampleOffset = (int32)(periodSize * 3);
                evOff.ppqPosition = 0;
                evOff.type = Event::kNoteOffEvent;
                evOff.flags = Event::kIsLive;
                evOff.noteOff.pitch = note;
                evOff.noteOff.velocity = 0;
                evOff.noteOff.noteId = an.noteId;
                eventList->addEvent (evOff);
            }
        }

        //--------------------------------------------------------------
        // Prepare ProcessData
        //--------------------------------------------------------------
        ProcessData processData;
        processData.processMode = kRealtime;
        processData.processContext = nullptr;
        processData.numSamples = (int32) periodSize;
        processData.symbolicSampleSize = kSample32;

        // Output buffers
        AudioBusBuffers outBuffers;
        outBuffers.numChannels = channels;
        outBuffers.silenceFlags = 0;
        outBuffers.channelBuffers32 = new float*[channels];
        for (unsigned i = 0; i < channels; ++i)
            outBuffers.channelBuffers32[i] = &audioBuf[i * periodSize];

        processData.outputs = &outBuffers;
        processData.numOutputs = 1;

        // Input buffers (none)
        AudioBusBuffers inBuffers;
        inBuffers.numChannels = 0;
        inBuffers.channelBuffers32 = nullptr;
        processData.inputs = &inBuffers;
        processData.numInputs = 0;

        // Event input
        processData.inputEvents = eventList.get ();
        processData.outputEvents = nullptr;

        // Parameter changes (none)
        processData.inputParameterChanges = initialParamsPending ? &initialParamChanges : nullptr;
        processData.outputParameterChanges = nullptr;

        // Zero output buffers before processing (avoid garbage)
        std::memset (audioBuf.data (), 0, periodSize * channels * sizeof (float));

        //--------------------------------------------------------------
        // Process audio
        //--------------------------------------------------------------
        tresult res = audioProcessor->process (processData);
        if (res != kResultTrue)
        {
            std::cerr << "process() returned " << res << std::endl;
        }

        if (initialParamsPending)
        {
            initialParamChanges.clearQueue ();
            initialParamsPending = false;
        }

        //--------------------------------------------------------------
        // Write audio to ALSA
        //--------------------------------------------------------------
        audio.writeFloat (audioBuf.data (), (int) periodSize);

        // Clean up output buffers
        delete[] outBuffers.channelBuffers32;

        // Clear event list for next block
        eventList->clear ();
    }

    //--------------------------------------------------------------
    // Cleanup
    //--------------------------------------------------------------
    if (plugView)
    {
        plugView->removed ();
        plugView->setFrame (nullptr);
        plugView->release ();
        plugView = nullptr;
    }
    plugFrame = nullptr;
    if (component)
    {
        component->setActive (false);
        component->terminate ();
    }
    if (controller)
        controller->terminate ();

    term.disable ();
    audio.close ();
    x11.close ();

    return 0;
}
