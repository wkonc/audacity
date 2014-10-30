/**********************************************************************

  Audacity: A Digital Audio Editor

  VSTEffect.cpp

  Dominic Mazzoni

  This class implements a VST Plug-in effect.  The plug-in must be
  loaded in a platform-specific way and passed into the constructor,
  but from here this class handles the interfacing.

**********************************************************************/

// *******************************************************************
// WARNING:  This is NOT 64-bit safe
// *******************************************************************

#if defined(BUILDING_AUDACITY)
#include "../../Audacity.h"
#include "../../PlatformCompatibility.h"

// Make the main function private
#define MODULEMAIN_SCOPE static
#else
#define MODULEMAIN_SCOPE
#define USE_VST 1
#endif

#if USE_VST

#include <wx/app.h>
#include <wx/defs.h>
#include <wx/buffer.h>
#include <wx/button.h>
#include <wx/combobox.h>
#include <wx/dcclient.h>
#include <wx/dialog.h>
#include <wx/filename.h>
#include <wx/frame.h>
#include <wx/imaglist.h>
#include <wx/listctrl.h>
#include <wx/log.h>
#include <wx/module.h>
#include <wx/msgdlg.h>
#include <wx/process.h>
#include <wx/recguard.h>
#include <wx/sizer.h>
#include <wx/slider.h>
#include <wx/scrolwin.h>
#include <wx/sstream.h>
#include <wx/statbox.h>
#include <wx/stattext.h>
#include <wx/timer.h>
#include <wx/tokenzr.h>
#include <wx/utils.h>

#include <vector>

#if defined(__WXMAC__)
#include <dlfcn.h>
#include <wx/mac/private.h>
#elif defined(__WXMSW__)
#include <wx/dynlib.h>
#include <wx/msw/seh.h>
#include <shlwapi.h>
#pragma comment(lib, "shlwapi")
#else
// Includes for GTK are later since they cause conflicts with our class names
#endif

// TODO:  Unfortunately we have some dependencies on Audacity provided 
//        dialogs, widgets and other stuff.  This will need to be cleaned up.

#include "FileDialog.h"
#include "../../FileNames.h"
#include "../../Internat.h"
#include "../../PlatformCompatibility.h"
#include "../../Prefs.h"
#include "../../ShuttleGui.h"
#include "../../effects/Effect.h"
#include "../../widgets/valnum.h"
#include "../../xml/XMLFileReader.h"
#include "../../xml/XMLWriter.h"

#include "audacity/ConfigInterface.h"

// Must include after ours since we have a lot of name collisions
#if defined(__WXGTK__)
#include <dlfcn.h>
#define Region XRegion     // Conflicts with Audacity's Region structure
#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#undef Region
#endif

#include "VSTEffect.h"

// NOTE:  To debug the subprocess, use wxLogDebug and, on Windows, Debugview
//        from TechNet (Sysinternals).

// ============================================================================
//
// Module registration entry point
//
// This is the symbol that Audacity looks for when the module is built as a
// dynamic library.
//
// When the module is builtin to Audacity, we use the same function, but it is
// declared static so as not to clash with other builtin modules.
//
// ============================================================================
MODULEMAIN_SCOPE ModuleInterface *AudacityModule(ModuleManagerInterface *moduleManager,
                                                 const wxString *path)
{
   // Create our effects module and register
   return new VSTEffectsModule(moduleManager, path);
}

#if defined(BUILDING_AUDACITY)
// ============================================================================
//
// Register this as a builtin module
// 
// We also take advantage of the fact that wxModules are initialized before
// the wxApp::OnInit() method is called.  We check to see if Audacity was
// executed to scan a VST effect in a different process.
//
// ============================================================================
DECLARE_BUILTIN_MODULE(VSTBuiltin);

class VSTSubEntry : public wxModule
{
public:
   bool OnInit()
   {
      // Have we been started to check a plugin?
      if (wxTheApp && wxTheApp->argc == 3 && wxStrcmp(wxTheApp->argv[1], VSTCMDKEY) == 0)
      {
         // NOTE:  This can really hide failures, which is what we want for those pesky
         //        VSTs that are bad or that our support isn't currect.  But, it can also
         //        hide Audacity failures in the subprocess, so if you're having an unruley
         //        VST or odd Audacity failures, comment it out and you might get more info.
         wxHandleFatalExceptions();
         VSTEffectsModule::Check(wxTheApp->argv[2]);

         // Returning false causes default processing to display a message box, but we don't
         // want that so disable logging.
         wxLog::EnableLogging(false);
         return false;
      }

      return true;
   };

   void OnExit() {};

   DECLARE_DYNAMIC_CLASS(VSTSubEntry);
};
IMPLEMENT_DYNAMIC_CLASS(VSTSubEntry, wxModule);

#endif

//----------------------------------------------------------------------------
// VSTSubProcess
//----------------------------------------------------------------------------
#define OUTPUTKEY                L"<VSTLOADCHK>-"
#define KEY_ID                   L"ID"
#define KEY_NAME                 L"Name"
#define KEY_PATH                 L"Path"
#define KEY_VENDOR               L"Vendor"
#define KEY_VERSION              L"Version"
#define KEY_DESCRIPTION          L"Description"
#define KEY_EFFECTTYPE           L"EffectType"
#define KEY_INTERACTIVE          L"Interactive"

class VSTSubProcess : public wxProcess,
                      public EffectIdentInterface
{
public:
   VSTSubProcess()
   {
      Redirect();
   }

   // EffectClientInterface implementation

   PluginID GetID()
   {
      return mID;
   }

   wxString GetPath()
   {
      return mPath;
   }

   wxString GetName()
   {
      return mName;
   }

   wxString GetVendor()
   {
      return mVendor;
   }

   wxString GetVersion()
   {
      return mVersion;
   }

   wxString GetDescription()
   {
      return mDescription;
   }

   wxString GetFamily()
   {
      return VSTPLUGINTYPE;
   }

   EffectType GetType()
   {
      return mType;
   }

   bool IsInteractive()
   {
      return mInteractive;
   }

   bool IsDefault()
   {
      return false;
   }

   bool IsLegacy()
   {
      return false;
   }

   bool IsRealtimeCapable()
   {
      return false;
      return mType == EffectTypeProcess;
   }


public:
   PluginID mID;
   wxString mPath;
   wxString mName;
   wxString mVendor;
   wxString mVersion;
   wxString mDescription;
   EffectType mType;
   bool mInteractive;
};

// ============================================================================
//
// VSTEffectsModule
//
// ============================================================================
VSTEffectsModule::VSTEffectsModule(ModuleManagerInterface *moduleManager,
                                   const wxString *path)
{
   mModMan = moduleManager;
   if (path)
   {
      mPath = *path;
   }
}

VSTEffectsModule::~VSTEffectsModule()
{
}

// ============================================================================
// IdentInterface implementation
// ============================================================================

wxString VSTEffectsModule::GetID()
{
   // Can be anything, but this is a v4 UUID
   return wxT("c5520489-0253-418e-bdcd-daba3a227b28");
}

wxString VSTEffectsModule::GetPath()
{
   return mPath;
}

wxString VSTEffectsModule::GetName()
{
   return _("VST Effects");
}

wxString VSTEffectsModule::GetVendor()
{
   return _("The Audacity Team");
}

wxString VSTEffectsModule::GetVersion()
{
   // This "may" be different if this were to be maintained as a separate DLL
   return AUDACITY_VERSION_STRING;
}

wxString VSTEffectsModule::GetDescription()
{
   return _("Adds the ability to use VST effects in Audacity.");
}

// ============================================================================
// ModuleInterface implementation
// ============================================================================

bool VSTEffectsModule::Initialize()
{
   // Nothing to do here
   return true;
}

void VSTEffectsModule::Terminate()
{
   // Nothing to do here
   return;
}

bool VSTEffectsModule::AutoRegisterPlugins(PluginManagerInterface & pm)
{
   // We don't auto-register
   return true;
}

wxArrayString VSTEffectsModule::FindPlugins(PluginManagerInterface & pm)
{
   wxArrayString pathList;
   wxArrayString files;

   // Check for the VST_PATH environment variable
   wxString vstpath = wxString::FromUTF8(getenv("VST_PATH"));
   if (!vstpath.empty())
   {
      wxStringTokenizer tok(vstpath);
      while (tok.HasMoreTokens())
      {
         pathList.push_back(wxString(tok.GetNextToken()));
      }
   }

#if defined(__WXMAC__)  
#define VSTPATH wxT("/Library/Audio/Plug-Ins/VST")

   // Look in /Library/Audio/Plug-Ins/VST and $HOME/Library/Audio/Plug-Ins/VST
   pathList.push_back(VSTPATH);
   pathList.push_back(wxString::FromUTF8(getenv("HOME")) + VSTPATH);

   // Recursively search all paths for Info.plist files.  This will identify all
   // bundles.
   pm.FindFilesInPathList(wxT("Info.plist"), pathList, files, true);

   // Remove the 'Contents/Info.plist' portion of the names
   for (size_t i = 0; i < files.GetCount(); i++)
   {
      files[i] = wxPathOnly(wxPathOnly(files[i]));
      if (!files[i].EndsWith(wxT(".vst")))
      {
         files.RemoveAt(i--);
      }
   }

#elif defined(__WXMSW__)

   TCHAR dpath[MAX_PATH];
   TCHAR tpath[MAX_PATH];
   DWORD len;

   // Try HKEY_CURRENT_USER registry key first
   len = WXSIZEOF(tpath);
   if (SHRegGetUSValue(wxT("Software\\VST"),
                       wxT("VSTPluginsPath"),
                       NULL,
                       tpath,
                       &len,
                       FALSE,
                       NULL,
                       0) == ERROR_SUCCESS)
   {
      tpath[len] = 0;
      dpath[0] = 0;
      ExpandEnvironmentStrings(tpath, dpath, WXSIZEOF(dpath));
      pathList.push_back(dpath);
   }

   // Then try HKEY_LOCAL_MACHINE registry key
   len = WXSIZEOF(tpath);
   if (SHRegGetUSValue(wxT("Software\\VST"),
                       wxT("VSTPluginsPath"),
                       NULL,
                       tpath,
                       &len,
                       TRUE,
                       NULL,
                       0) == ERROR_SUCCESS)
   {
      tpath[len] = 0;
      dpath[0] = 0;
      ExpandEnvironmentStrings(tpath, dpath, WXSIZEOF(dpath));
      pathList.push_back(dpath);
   }

   // Add the default path last
   dpath[0] = 0;
   ExpandEnvironmentStrings(wxT("%ProgramFiles%\\Steinberg\\VSTPlugins"),
                            dpath,
                            WXSIZEOF(dpath));
   pathList.push_back(dpath);

   // Recursively scan for all DLLs
   pm.FindFilesInPathList(wxT("*.dll"), pathList, files, true);

#else

   // Nothing specified in the VST_PATH environment variable...provide defaults
   if (vstpath.IsEmpty())
   {
      // We add this "non-default" one
      pathList.Add(wxT(LIBDIR) wxT("/vst"));

      // These are the defaults used by other hosts
      pathList.Add(wxT("/usr/lib/vst"));
      pathList.Add(wxT("/usr/local/lib/vst"));
      pathList.Add(wxString(wxGetHomeDir()) + wxFILE_SEP_PATH + wxT(".vst"));
   }

   // Recursively scan for all shared objects
   pm.FindFilesInPathList(wxT("*.so"), pathList, files, true);

#endif

   return files;
}

bool VSTEffectsModule::RegisterPlugin(PluginManagerInterface & pm, const wxString & path)
{
   // TODO:  Fix this for external usage
   wxString cmdpath = PlatformCompatibility::GetExecutablePath();

   wxString cmd;
   cmd.Printf(wxT("\"%s\" %s \"%s\""), cmdpath.c_str(), VSTCMDKEY, path.c_str());

   VSTSubProcess *proc = new VSTSubProcess();
   try
   {
      wxExecute(cmd, wxEXEC_SYNC | wxEXEC_NODISABLE, proc);
   }
   catch (...)
   {
      wxLogMessage(_("VST plugin registration failed for %s\n"), path.c_str());
      return false;
   }

   wxString output;
   wxStringOutputStream ss(&output);
   proc->GetInputStream()->Read(ss);

   int keycount = 0;
   wxStringTokenizer tzr(output, wxT("\n"));
   while (tzr.HasMoreTokens())
   {
      wxString line = tzr.GetNextToken();

      // Our output may follow any output the plugin may have written.
      if (!line.StartsWith(OUTPUTKEY))
      {
         continue;
      }

      wxString key = line.Mid(wxStrlen(OUTPUTKEY)).BeforeFirst(wxT('='));
      wxString val = line.AfterFirst(wxT('=')).BeforeFirst(wxT('\r'));

      if (key.IsSameAs(KEY_ID))
      {
         proc->mID = val;
         keycount++;
      }
      else if (key.IsSameAs(KEY_NAME))
      {
         proc->mName = val;
         keycount++;
      }
      else if (key.IsSameAs(KEY_PATH))
      {
         proc->mPath = val;
         keycount++;
      }
      else if (key.IsSameAs(KEY_VENDOR))
      {
         proc->mVendor = val;
         keycount++;
      }
      else if (key.IsSameAs(KEY_VERSION))
      {
         proc->mVersion = val;
         keycount++;
      }
      else if (key.IsSameAs(KEY_DESCRIPTION))
      {
         proc->mDescription = val;
         keycount++;
      }
      else if (key.IsSameAs(KEY_EFFECTTYPE))
      {
         long type;
         val.ToLong(&type);
         proc->mType = (EffectType) type;
         keycount++;
      }
      else if (key.IsSameAs(KEY_INTERACTIVE))
      {
         proc->mInteractive = val.IsSameAs(wxT("1"));
         keycount++;
      }
   }

   bool valid = keycount == 8;

   if (valid)
   {
      pm.RegisterEffectPlugin(this, proc);
   }

   delete proc;

   return valid;
}

void *VSTEffectsModule::CreateInstance(const PluginID & WXUNUSED(ID),
                                       const wxString & path)
{
   // For us, the ID is simply the path to the effect
   return new VSTEffect(path);
}

// ============================================================================
// ModuleEffectInterface implementation
// ============================================================================

// ============================================================================
// VSTEffectsModule implementation
// ============================================================================

// static
//
// Called from reinvokation of Audacity or DLL to check in a separate process
void VSTEffectsModule::Check(const wxChar *path)
{
   VSTEffect *effect = new VSTEffect(path);
   if (effect)
   {
      if (effect->Startup())
      {
         wxPrintf(OUTPUTKEY KEY_ID wxT("=%s\n"), effect->GetID().c_str());
         wxPrintf(OUTPUTKEY KEY_PATH wxT("=%s\n"), effect->GetPath().c_str());
         wxPrintf(OUTPUTKEY KEY_NAME wxT("=%s\n"), effect->GetName().c_str());
         wxPrintf(OUTPUTKEY KEY_VENDOR wxT("=%s\n"), effect->GetVendor().c_str());
         wxPrintf(OUTPUTKEY KEY_VERSION wxT("=%s\n"), effect->GetVersion().c_str());
         wxPrintf(OUTPUTKEY KEY_DESCRIPTION wxT("=%s\n"), effect->GetDescription().c_str());
         wxPrintf(OUTPUTKEY KEY_EFFECTTYPE wxT("=%d\n"), effect->GetType());
         wxPrintf(OUTPUTKEY KEY_INTERACTIVE wxT("=%d\n"), effect->IsInteractive());
      }

      delete effect;
   }
}

///////////////////////////////////////////////////////////////////////////////
//
// VSTEffectSettingsDialog
//
///////////////////////////////////////////////////////////////////////////////

class VSTEffectSettingsDialog:public wxDialog
{
public:
   VSTEffectSettingsDialog(wxWindow * parent, EffectHostInterface *host);
   virtual ~VSTEffectSettingsDialog();

   void PopulateOrExchange(ShuttleGui & S);

   void OnOk(wxCommandEvent & evt);

private:
   EffectHostInterface *mHost;
   int mBufferSize;
   bool mUseBufferDelay;
   bool mUseGUI;
   bool mRescan;

   DECLARE_EVENT_TABLE()
};

BEGIN_EVENT_TABLE(VSTEffectSettingsDialog, wxDialog)
   EVT_BUTTON(wxID_OK, VSTEffectSettingsDialog::OnOk)
END_EVENT_TABLE()

VSTEffectSettingsDialog::VSTEffectSettingsDialog(wxWindow * parent, EffectHostInterface *host)
:  wxDialog(parent, wxID_ANY, wxString(_("VST Effect Settings")))
{
#if defined(EXPERIMENTAL_REALTIME_EFFECTS) && defined(__WXMAC__)
   HIWindowChangeClass((WindowRef) MacGetWindowRef(), kMovableModalWindowClass);
#endif

   mHost = host;

   mHost->GetSharedConfig(wxT("Settings"), wxT("BufferSize"), mBufferSize, 8192);
   mHost->GetSharedConfig(wxT("Settings"), wxT("UseBufferDelay"), mUseBufferDelay, true);
   mHost->GetSharedConfig(wxT("Settings"), wxT("UseGUI"), mUseGUI, true);
   mHost->GetSharedConfig(wxT("Settings"), wxT("Rescan"), mRescan, false);

   ShuttleGui S(this, eIsCreating);
   PopulateOrExchange(S);
}

VSTEffectSettingsDialog::~VSTEffectSettingsDialog()
{
}

void VSTEffectSettingsDialog::PopulateOrExchange(ShuttleGui & S)
{
   S.SetBorder(5);
   S.StartHorizontalLay(wxEXPAND, 1);
   {
      S.StartVerticalLay(false);
      {
         S.StartStatic(_("Buffer Size"));
         {
            wxIntegerValidator<int> vld(&mBufferSize);
            vld.SetRange(8, 1048576 * 1);

            S.AddVariableText(wxString() +
               _("The buffer size controls the number of samples sent to the effect ") +
               _("on each iteration. Smaller values will cause slower processing and ") +
               _("some effects require 8192 samples or less to work properly. However ") +
               _("most effects can accept large buffers and using them will greatly ") +
               _("reduce processing time."))->Wrap(650);

            S.StartHorizontalLay(wxALIGN_LEFT);
            {
               wxTextCtrl *t;
               t = S.TieNumericTextBox(_("&Buffer Size (8 to 1048576 samples):"),
                                       mBufferSize,
                                       12);
               t->SetMinSize(wxSize(100, -1));
               t->SetValidator(vld);
            }
            S.EndHorizontalLay();
         }
         S.EndStatic();

         S.StartStatic(_("Buffer Delay Compensation"));
         {
            S.AddVariableText(wxString() +
               _("As part of their processing, some VST effects must delay returning ") +
               _("audio to Audacity. When not compensating for this delay, you will ") +
               _("notice that small silences have been inserted into the audio. ") +
               _("Enabling this setting will provide that compensation, but it may ") +
               _("not work for all VST effects."))->Wrap(650);

            S.StartHorizontalLay(wxALIGN_LEFT);
            {
               S.TieCheckBox(_("Enable &compensation"),
                             mUseBufferDelay);
            }
            S.EndHorizontalLay();
         }
         S.EndStatic();

         S.StartStatic(_("Graphical Mode"));
         {
            S.AddVariableText(wxString() +
               _("Most VST effects have a graphical interface for setting parameter values.") +
               _(" A basic text-only method is also available. ") +
               _(" Reopen the effect for this to take effect."))->Wrap(650);
            S.TieCheckBox(_("Enable &graphical interface"),
                          mUseGUI);
         }
         S.EndStatic();

         S.StartStatic(_("Rescan Effects"));
         {
            S.AddVariableText(wxString() +
               _("To improve Audacity startup, a search for VST effects is performed ") +
               _("once and relevant information is recorded. When you add VST effects ") +
               _("to your system, you need to tell Audacity to rescan so the new ") +
               _("information can be recorded."))->Wrap(650);
            S.TieCheckBox(_("&Rescan effects on next launch"),
                          mRescan);
         }
         S.EndStatic();
      }
      S.EndVerticalLay();
   }
   S.EndHorizontalLay();

   S.AddStandardButtons();

   Layout();
   Fit();
   Center();
}

void VSTEffectSettingsDialog::OnOk(wxCommandEvent & WXUNUSED(evt))
{
   if (!Validate())
   {
      return;
   }

   ShuttleGui S(this, eIsGettingFromDialog);
   PopulateOrExchange(S);

   mHost->SetSharedConfig(wxT("Settings"), wxT("BufferSize"), mBufferSize);
   mHost->SetSharedConfig(wxT("Settings"), wxT("UseBufferDelay"), mUseBufferDelay);
   mHost->SetSharedConfig(wxT("Settings"), wxT("UseGUI"), mUseGUI);
   mHost->SetSharedConfig(wxT("Settings"), wxT("Rescan"), mRescan);

   EndModal(wxID_OK);
}

///////////////////////////////////////////////////////////////////////////////
//
// VSTEffectDialog
//
///////////////////////////////////////////////////////////////////////////////
DECLARE_LOCAL_EVENT_TYPE(EVT_SIZEWINDOW, -1);
DEFINE_LOCAL_EVENT_TYPE(EVT_SIZEWINDOW);

DECLARE_LOCAL_EVENT_TYPE(EVT_UPDATEDISPLAY, -1);
DEFINE_LOCAL_EVENT_TYPE(EVT_UPDATEDISPLAY);

class VSTEffectDialog:public wxDialog, XMLTagHandler
{
public:
   VSTEffectDialog(wxWindow * parent,
                   const wxString & title,
                   VSTEffect *effect,
                   AEffect *aeffect);
   virtual ~VSTEffectDialog();

   void EnableApply(bool enable);

private:

   void RemoveHandler();

   void OnProgram(wxCommandEvent & evt);
   void OnProgramText(wxCommandEvent & evt);
   void OnLoad(wxCommandEvent & evt);
   void OnSave(wxCommandEvent & evt);
   void OnSettings(wxCommandEvent & evt);

   void OnSlider(wxCommandEvent & evt);

#if defined(EXPERIMENTAL_REALTIME_EFFECTS)
   void OnApply(wxCommandEvent & evt);
#else
   void OnOk(wxCommandEvent & evt);
   void OnCancel(wxCommandEvent & evt);
   void OnPreview(wxCommandEvent & evt);
#endif   

   void OnDefaults(wxCommandEvent & evt);
   void OnClose(wxCloseEvent & evt);

   void OnSizeWindow(wxCommandEvent & evt);
   void OnUpdateDisplay(wxCommandEvent & evt);

   void BuildPlain();
   void BuildFancy();
   wxSizer *BuildProgramBar();
   void RefreshParameters(int skip = -1);

   // Program/Bank loading/saving
   bool LoadFXB(const wxFileName & fn);
   bool LoadFXP(const wxFileName & fn);
   bool LoadXML(const wxFileName & fn);
   bool LoadFXProgram(unsigned char **bptr, ssize_t & len, int index, bool dryrun);
   void SaveFXB(const wxFileName & fn);
   void SaveFXP(const wxFileName & fn);
   void SaveXML(const wxFileName & fn);
   void SaveFXProgram(wxMemoryBuffer & buf, int index);

   virtual bool HandleXMLTag(const wxChar *tag, const wxChar **attrs);
   virtual void HandleXMLEndTag(const wxChar *tag);
   virtual void HandleXMLContent(const wxString & content);
   virtual XMLTagHandler *HandleXMLChild(const wxChar *tag);

private:

   VSTEffect *mEffect;
   AEffect *mAEffect;

   bool mGui;

   wxSizerItem *mContainer;

   wxComboBox *mProgram;
   wxStaticText **mNames;
   wxSlider **mSliders;
   wxStaticText **mDisplays;
   wxStaticText **mLabels;

   bool mInChunk;
   wxString mChunk;

#if defined(__WXMAC__)
   static pascal OSStatus OverlayEventHandler(EventHandlerCallRef handler, EventRef event, void *data);
   OSStatus OnOverlayEvent(EventHandlerCallRef handler, EventRef event);
   static pascal OSStatus WindowEventHandler(EventHandlerCallRef handler, EventRef event, void *data);
   OSStatus OnWindowEvent(EventHandlerCallRef handler, EventRef event);

   WindowRef mOverlayRef;
   EventHandlerUPP mOverlayEventHandlerUPP;
   EventHandlerRef mOverlayEventHandlerRef;

   WindowRef mWindowRef;
   WindowRef mPreviousRef;
   EventHandlerUPP mWindowEventHandlerUPP;
   EventHandlerRef mWindowEventHandlerRef;

#elif defined(__WXMSW__)

   HANDLE mHwnd;

#else

   Display *mXdisp;
   Window mXwin;

#endif

   DECLARE_EVENT_TABLE()
};

enum
{
   ID_VST_PROGRAM = 11000,
   ID_VST_LOAD,
   ID_VST_SAVE,
   ID_VST_SLIDERS,
   ID_VST_SETTINGS
};

BEGIN_EVENT_TABLE(VSTEffectDialog, wxDialog)
   EVT_CLOSE(VSTEffectDialog::OnClose)

#if defined(EXPERIMENTAL_REALTIME_EFFECTS)
   EVT_BUTTON(wxID_APPLY, VSTEffectDialog::OnApply)
#else
   EVT_BUTTON(wxID_OK, VSTEffectDialog::OnOk)
   EVT_BUTTON(wxID_CANCEL, VSTEffectDialog::OnCancel)
   EVT_BUTTON(ID_EFFECT_PREVIEW, VSTEffectDialog::OnPreview)
#endif

   EVT_BUTTON(eDefaultsID, VSTEffectDialog::OnDefaults)

   EVT_COMBOBOX(ID_VST_PROGRAM, VSTEffectDialog::OnProgram)
   EVT_TEXT(ID_VST_PROGRAM, VSTEffectDialog::OnProgramText)
   EVT_BUTTON(ID_VST_LOAD, VSTEffectDialog::OnLoad)
   EVT_BUTTON(ID_VST_SAVE, VSTEffectDialog::OnSave)
   EVT_BUTTON(ID_VST_SETTINGS, VSTEffectDialog::OnSettings)

   EVT_SLIDER(wxID_ANY, VSTEffectDialog::OnSlider)

   // Events from the audioMaster callback
   EVT_COMMAND(wxID_ANY, EVT_SIZEWINDOW, VSTEffectDialog::OnSizeWindow)
   EVT_COMMAND(wxID_ANY, EVT_UPDATEDISPLAY, VSTEffectDialog::OnUpdateDisplay)
END_EVENT_TABLE()

#if defined(__WXMAC__)

// To use, change the SDK in the project to at least 10.5
extern void DebugPrintControlHierarchy(WindowRef inWindow);
extern void DebugPrintWindowList(void);

// ----------------------------------------------------------------------------
// Most of the following is used to deal with VST effects that create an overlay
// window on top of ours.  This is usually done because Cocoa is being used
// instead of Carbon.
//
// That works just fine...usually.  But, we display the effect in a modal dialog
// box and, since that overlay window is just another window in the application,
// the modality of the dialog causes the overlay to be disabled and the user
// can't interact with the effect.
//
// Examples of these effects would be BlueCat's Freeware Pack and GRM Tools,
// though I'm certain there are other's out there.  Anything JUCE based would
// affected...that's what GRM Tools uses.
//
// So, to work around the problem (without moving to Cocoa or wxWidgets 3.x),
// we install an event handler if the overlay is detected.  This handler and
// the companion handler on our window use the kEventWindowGetClickModality
// event to tell the system that events can be passed to our window and the
// overlay window.
//
// In addition, there's some window state management that must be dealt with
// to keep our window from becoming unhightlighted when the floater is clicked.
// ----------------------------------------------------------------------------

// Events to be captured in the overlay window
static const EventTypeSpec OverlayEventList[] =
{
#if !defined(EXPERIMENTAL_REALTIME_EFFECTS)
   { kEventClassWindow, kEventWindowGetClickModality },
#endif
#if 0
   { kEventClassMouse,  kEventMouseDown },
   { kEventClassMouse,  kEventMouseUp },
   { kEventClassMouse,  kEventMouseMoved },
   { kEventClassMouse,  kEventMouseDragged },
   { kEventClassMouse,  kEventMouseEntered },
   { kEventClassMouse,  kEventMouseExited },
   { kEventClassMouse,  kEventMouseWheelMoved },
#endif
};

// Overlay window event handler callback thunk
pascal OSStatus VSTEffectDialog::OverlayEventHandler(EventHandlerCallRef handler, EventRef event, void *data)
{
   return ((VSTEffectDialog *)data)->OnOverlayEvent(handler, event);
}

// Overlay window event handler
OSStatus VSTEffectDialog::OnOverlayEvent(EventHandlerCallRef handler, EventRef event)
{
   // Get the current window in front of all the rest of the non-floaters.
   WindowRef frontwin = FrontNonFloatingWindow();

   // Get the target window of the event
   WindowRef evtwin = 0;
   GetEventParameter(event,
                     kEventParamDirectObject,
                     typeWindowRef,
                     NULL,
                     sizeof(evtwin),
                     NULL,
                     &evtwin);
#define DEBUG_VST
#if defined(DEBUG_VST)
   int cls = GetEventClass(event);
   printf("OVERLAY class %4.4s kind %d ewin %p owin %p mwin %p anf %p fnf %p\n",
      &cls,
      GetEventKind(event),
      evtwin,
      mOverlayRef,
      mWindowRef,
      ActiveNonFloatingWindow(),
      frontwin);
#endif

   bool block = false;
   WindowModality kind;
   WindowRef ref = NULL;
   GetWindowModality(frontwin, &kind, &ref);

   switch (kind)
   {
      case kWindowModalityNone:
      {
         // Allow
      }
      break;

      case kWindowModalityWindowModal:
      {
         if (ref == mWindowRef || ref == mOverlayRef)
         {
            block = true;
         }
      }
      break;

      case kWindowModalitySystemModal:
      case kWindowModalityAppModal:
      {
         if (frontwin != mWindowRef && frontwin != mOverlayRef)
         {
            block = true;
         }
      }
      break;
   }

   // We must block mouse events because plugins still act on mouse
   // movement and drag events, even if they are supposed to be disabled
   // due to other modal dialogs (like when Load or Settings are clicked).
   if (GetEventClass(event) == kEventClassMouse)
   {
      if (block)
      {
         return noErr;
      }
      
      return eventNotHandledErr;
   }

   // Only kEventClassWindow events at this point
   switch (GetEventKind(event))
   {
      // The system is asking if the target of an upcoming event
      // should be passed to the overlay window or not.
      //
      // We allow it when the overlay window or our window is the
      // curret top window.  Any other windows would mean that a
      // modal dialog box has been opened on top and we should block.
      case kEventWindowGetClickModality:
      {
         // Announce the event may need blocking
         HIModalClickResult res = block ? kHIModalClickIsModal | kHIModalClickAnnounce : 0;

         // Set the return parameters
         SetEventParameter(event,
                           kEventParamWindowModality,
                           typeWindowRef,
                           sizeof(kind),
                           &kind);

         SetEventParameter(event,
                           kEventParamModalWindow,
                           typeWindowRef,
                           sizeof(ref),
                           &ref);

         SetEventParameter(event,
                           kEventParamModalClickResult,
                           typeModalClickResult,
                           sizeof(res),
                           &res);

#if !defined(EXPERIMENTAL_REALTIME_EFFECTS)
         // If the front window is the overlay, then make our window
         // the selected one so that the mouse click goes to it instead.
         if (frontwin == mOverlayRef)
         {
            SelectWindow(mWindowRef);
         }
#endif
         return noErr;
      }
      break;
   }

   return eventNotHandledErr;
}

// Events to be captured in the our window
static const EventTypeSpec WindowEventList[] =
{
   { kEventClassWindow, kEventWindowGetClickModality },
   { kEventClassWindow, kEventWindowShown },
   { kEventClassWindow, kEventWindowClose },
#if 0
   { kEventClassMouse,  kEventMouseDown },
   { kEventClassMouse,  kEventMouseUp },
   { kEventClassMouse,  kEventMouseMoved },
   { kEventClassMouse,  kEventMouseDragged },
   { kEventClassMouse,  kEventMouseEntered },
   { kEventClassMouse,  kEventMouseExited },
   { kEventClassMouse,  kEventMouseWheelMoved },
#endif
};

// Our window event handler callback thunk
pascal OSStatus VSTEffectDialog::WindowEventHandler(EventHandlerCallRef handler, EventRef event, void *data)
{
   return ((VSTEffectDialog *)data)->OnWindowEvent(handler, event);
}

// Our window event handler
OSStatus VSTEffectDialog::OnWindowEvent(EventHandlerCallRef handler, EventRef event)
{
   // Get the current window in from of all the rest non-floaters.
   WindowRef frontwin = FrontNonFloatingWindow();

   // Get the target window of the event
   WindowRef evtwin = 0;
   GetEventParameter(event,
                     kEventParamDirectObject,
                     typeWindowRef,
                     NULL,
                     sizeof(evtwin),
                     NULL,
                     &evtwin);

#if defined(DEBUG_VST)
   int cls = GetEventClass(event);
   printf("WINDOW class %4.4s kind %d ewin %p owin %p mwin %p anf %p fnf %p\n",
      &cls,
      GetEventKind(event),
      evtwin,
      mOverlayRef,
      mWindowRef,
      ActiveNonFloatingWindow(),
      frontwin);
#endif

   bool block = false;
   WindowModality kind;
   WindowRef ref = NULL;
   GetWindowModality(frontwin, &kind, &ref);

   switch (kind)
   {
      case kWindowModalityNone:
      {
         // Allow
      }
      break;

      case kWindowModalityWindowModal:
      {
         if (ref == mWindowRef || ref == mOverlayRef)
         {
            block = true;
         }
      }
      break;

      case kWindowModalitySystemModal:
      case kWindowModalityAppModal:
      {
         if (frontwin != mWindowRef && frontwin != mOverlayRef)
         {
            block = true;
         }
      }
      break;
   }

   // We must block mouse events because plugins still act on mouse
   // movement and drag events, even if they are supposed to be disabled
   // due to other modal dialogs (like when Load or Settings are clicked).
   if (GetEventClass(event) == kEventClassMouse)
   {
      if (block)
      {
         return noErr;
      }

      return eventNotHandledErr;
   }

   // Only kEventClassWindow events at this point
   switch (GetEventKind(event))
   {
      // If we don't capture the close event, Audacity will crash at termination
      // since the window is still on the wxWidgets toplevel window lists, but
      // it has already been deleted from the system.
      case kEventWindowClose:
      {
         RemoveHandler();
         Close();
         return noErr;
      }
      break;

      // This is where we determine if the effect has created a window above
      // ours.  Since the overlay is created on top of our window, we look at
      // the topmost window to see if it is different that ours.  If so, then
      // we assume an overlay has been created and install the event handler
      // on the overlay.
      case kEventWindowShown:
      {
         // Have an overlay?
         WindowRef newprev = GetPreviousWindow(mWindowRef);

         if (newprev != mPreviousRef)
         {
            // We have an overlay
            mOverlayRef = newprev;

            // Set our window's activatino scope to make sure it alway
            // stays active.
            SetWindowActivationScope(mWindowRef,
                                     kWindowActivationScopeIndependent);

            // Install the overlay handler
            mOverlayEventHandlerUPP = NewEventHandlerUPP(OverlayEventHandler);
            InstallWindowEventHandler(mOverlayRef,
                                      mOverlayEventHandlerUPP,
                                      GetEventTypeCount(OverlayEventList),
                                      OverlayEventList,
                                      this,
                                      &mOverlayEventHandlerRef);

//#if !defined(EXPERIMENTAL_REALTIME_EFFECTS)
            // Since we set the activation scope to independent,
            // we need to make sure the overlay gets activated.
            ActivateWindow(mOverlayRef, TRUE);
//#endif
         }
      }
      break;

      // The system is asking if the target of an upcoming event
      // should be passed to the overlay window or not.
      //
      // We allow it when the overlay window or our window is the
      // curret top window.  Any other windows would mean that a
      // modal dialog box has been opened on top and we should block.
      case kEventWindowGetClickModality:
      {
         // Announce the event may need blocking
         HIModalClickResult res = block ? kHIModalClickIsModal | kHIModalClickAnnounce : 0;

         // Set the return parameters
         SetEventParameter(event,
                           kEventParamWindowModality,
                           typeWindowRef,
                           sizeof(kind),
                           &kind);

         SetEventParameter(event,
                           kEventParamModalWindow,
                           typeWindowRef,
                           sizeof(ref),
                           &ref);

         SetEventParameter(event,
                           kEventParamModalClickResult,
                           typeModalClickResult,
                           sizeof(res),
                           &res);

         if (mOverlayRef)
         {
            // If the front window is the overlay, then make our window
            // the selected one so that the mouse click go to it instead.
            WindowRef act = ActiveNonFloatingWindow();
            if (frontwin == mOverlayRef || act == NULL || act == mOverlayRef)
            {
               SelectWindow(mWindowRef);
            }
         }

         return noErr;
      }
      break;
   }

   return eventNotHandledErr;
}
#endif

#if defined(__WXGTK__)

static int trappedErrorCode = 0;
static int X11TrapHandler(Display *, XErrorEvent *err)
{
    return 0;
}
#endif

VSTEffectDialog::VSTEffectDialog(wxWindow *parent,
                                 const wxString & title,
                                 VSTEffect *effect,
                                 AEffect *aeffect)
:  wxDialog(parent, wxID_ANY, title),
   mEffect(effect),
   mAEffect(aeffect)
{
   mNames = NULL;
   mSliders = NULL;
   mDisplays = NULL;
   mLabels = NULL;
   mContainer = NULL;

#if defined(__WXMAC__)

#if defined(EXPERIMENTAL_REALTIME_EFFECTS)
   HIWindowChangeClass((WindowRef) MacGetWindowRef(), kFloatingWindowClass);
#endif

   mOverlayRef = 0;
   mOverlayEventHandlerUPP = 0;
   mOverlayEventHandlerRef = 0;

   mWindowRef = 0;
   mWindowEventHandlerUPP = 0;
   mWindowEventHandlerRef = 0;
#elif defined(__WXMSW__)
   mHwnd = 0;
#else
   mXdisp = NULL;
   mXwin = NULL;
#endif

   // Determine if the VST editor is supposed to be used or not
   mEffect->mHost->GetSharedConfig(wxT("Settings"),
                                   wxT("UseGUI"),
                                   mGui,
                                   true);
   mGui = mAEffect->flags & effFlagsHasEditor ? mGui : false;

   // Must use the GUI editor if parameters aren't provided
   if (mAEffect->numParams == 0)
   {
      mGui = true;
   }

   // Build the appropriate dialog type
   if (mGui)
   {
      BuildFancy();
   }
   else
   {
      BuildPlain();
   }
}

VSTEffectDialog::~VSTEffectDialog()
{
   mEffect->InterfaceClosed();

   mEffect->PowerOff();
   mEffect->NeedEditIdle(false);

   RemoveHandler();

   if (mNames)
   {
      delete [] mNames;
   }

   if (mSliders)
   {
      delete [] mSliders;
   }

   if (mDisplays)
   {
      delete [] mDisplays;
   }

   if (mLabels)
   {
      delete [] mLabels;
   }
}

void VSTEffectDialog::RemoveHandler()
{
#if defined(__WXMAC__)
   if (mWindowRef)
   {
      mEffect->callDispatcher(effEditClose, 0, 0, mWindowRef, 0.0);
      mWindowRef = 0;
   }

   if (mOverlayEventHandlerRef)
   {
      ::RemoveEventHandler(mOverlayEventHandlerRef);
      mOverlayEventHandlerRef = 0;
   }

   if (mOverlayEventHandlerUPP)
   {
      DisposeEventHandlerUPP(mOverlayEventHandlerUPP);
      mOverlayEventHandlerUPP = 0;
   }

   if (mWindowEventHandlerRef)
   {
      ::RemoveEventHandler(mWindowEventHandlerRef);
      mWindowEventHandlerRef = 0;
      MacInstallTopLevelWindowEventHandler();
   }

   if (mWindowEventHandlerUPP)
   {
      DisposeEventHandlerUPP(mWindowEventHandlerUPP);
      mWindowEventHandlerUPP = 0;
   }
#elif defined(__WXMSW__)
   if (mHwnd)
   {
      mEffect->callDispatcher(effEditClose, 0, 0, mHwnd, 0.0);
      mHwnd = 0;
   }
#else
   if (mXwin)
   {
      mEffect->callDispatcher(effEditClose, 0, (intptr_t)mXdisp, (void *)mXwin, 0.0);
      mXdisp = NULL;
      mXwin = NULL;
   }
#endif
}

void VSTEffectDialog::BuildFancy()
{
   struct
   {
      short top, left, bottom, right;
   } *rect;

   // Turn the power on...some effects need this when the editor is open
   mEffect->PowerOn();

   // Some effects like to have us get their rect before opening them.
   mEffect->callDispatcher(effEditGetRect, 0, 0, &rect, 0.0);

#if defined(__WXMAC__)
   // Retrieve the current window and the one above it.  The window list
   // is kept in top-most to bottom-most order, so we'll use that to
   // determine if another window was opened above ours.
   mWindowRef = (WindowRef) MacGetWindowRef();
   mPreviousRef = GetPreviousWindow(mWindowRef);

   // Install the event handler on our window
   mWindowEventHandlerUPP = NewEventHandlerUPP(WindowEventHandler);
   InstallWindowEventHandler(mWindowRef,
                             mWindowEventHandlerUPP,
                             GetEventTypeCount(WindowEventList),
                             WindowEventList,
                             this,
                             &mWindowEventHandlerRef);

   // Find the content view within our window
   HIViewRef view;
   HIViewFindByID(HIViewGetRoot(mWindowRef), kHIViewWindowContentID, &view);

   // And ask the effect to add it's GUI
   mEffect->callDispatcher(effEditOpen, 0, 0, mWindowRef, 0.0);

   // Get the subview it created
   HIViewRef subview = HIViewGetFirstSubview(view);
   if (subview == NULL)
   {
      // Doesn't seem the effect created the subview, so switch
      // to the plain dialog.  This can happen when an effect
      // uses the content view directly.  As of this time, we
      // will not try to support those and fall back to the
      // textual interface.
      mGui = false;
      RemoveHandler();
      BuildPlain();
      return;
   }

#elif defined(__WXMSW__)

   wxPanel *w = new wxPanel(this, wxID_ANY);
   mHwnd = w->GetHWND();
   mEffect->callDispatcher(effEditOpen, 0, 0, mHwnd, 0.0);

#else

   // Use a panel to host the plugins GUI
   wxPanel *w = new wxPanel(this);

   // Make sure is has a window
   if (!GTK_WIDGET(w->m_wxwindow)->window)
   {
      gtk_widget_realize(GTK_WIDGET(w->m_wxwindow));
   }

   GdkWindow *gwin = GTK_WIDGET(w->m_wxwindow)->window;
   mXdisp = GDK_WINDOW_XDISPLAY(gwin);
   mXwin = GDK_WINDOW_XWINDOW(gwin);

   mEffect->callDispatcher(effEditOpen, 0, (intptr_t)mXdisp, (void *)mXwin, 0.0);

#endif

   // Get the final bounds of the effect GUI
   mEffect->callDispatcher(effEditGetRect, 0, 0, &rect, 0.0);

   // Build our display now
   wxBoxSizer *vs = new wxBoxSizer(wxVERTICAL);
   wxBoxSizer *hs = new wxBoxSizer(wxHORIZONTAL);

   // Add the program bar at the top
   vs->Add(BuildProgramBar(), 0, wxCENTER | wxEXPAND);

#if defined(__WXMAC__)

   // Reserve space for the effect GUI
   mContainer = hs->Add(rect->right - rect->left, rect->bottom - rect->top);

#elif defined(__WXMSW__)

   // Add the effect host window to the layout
   mContainer = hs->Add(w, 1, wxCENTER | wxEXPAND);
   mContainer->SetMinSize(rect->right - rect->left, rect->bottom - rect->top);

#else

   // Add the effect host window to the layout
   mContainer = hs->Add(w, 1, wxCENTER | wxEXPAND);
   mContainer->SetMinSize(rect->right - rect->left, rect->bottom - rect->top);

#endif

   vs->Add(hs, 0, wxCENTER);

   // Add the standard button bar at the bottom
#if defined(EXPERIMENTAL_REALTIME_EFFECTS)
   vs->Add(CreateStdButtonSizer(this, eApplyButton | eDefaultsButton), 0, wxEXPAND);
#else
   vs->Add(CreateStdButtonSizer(this, ePreviewButton | eDefaultsButton |eCancelButton | eOkButton), 0, wxEXPAND);
#endif
   SetSizerAndFit(vs);

#if defined(__WXMAC__)

   // Found out where the reserved space wound up
   wxPoint pos = mContainer->GetPosition();

   // Reposition the subview into the reserved space
   HIViewPlaceInSuperviewAt(subview, pos.x, pos.y);

   // Some VST effects do not work unless the default handler is removed since
   // it captures many of the events that the plugins need.  But, it must be
   // done last since proper window sizing will not occur otherwise.
   ::RemoveEventHandler((EventHandlerRef)MacGetEventHandler());

#elif defined(__WXMSW__)
#else
#endif

   mEffect->NeedEditIdle(true);
}

void VSTEffectDialog::BuildPlain()
{
   mNames = new wxStaticText *[mAEffect->numParams];
   mSliders = new wxSlider *[mAEffect->numParams];
   mDisplays = new wxStaticText *[mAEffect->numParams];
   mLabels = new wxStaticText *[mAEffect->numParams];

   wxBoxSizer *vSizer = new wxBoxSizer(wxVERTICAL);
   vSizer->Add(BuildProgramBar(), 0,  wxALIGN_CENTER | wxEXPAND);

   wxScrolledWindow *sw = new wxScrolledWindow(this,
                                               wxID_ANY,
                                               wxDefaultPosition,
                                               wxDefaultSize,
                                               wxVSCROLL | wxTAB_TRAVERSAL);

   // Try to give the window a sensible default/minimum size
   wxSize sz = GetParent()->GetSize();
   sw->SetMinSize(wxSize(wxMax(600, sz.GetWidth() * 2 / 3), sz.GetHeight() / 2));

   sw->SetScrollRate(0, 20);
   vSizer->Add(sw, 1, wxEXPAND | wxALL, 5);

   // Add the standard button bar at the bottom
#if defined(EXPERIMENTAL_REALTIME_EFFECTS)
   vSizer->Add(CreateStdButtonSizer(this, eApplyButton | eDefaultsButton), 0, wxEXPAND);
#else
   vSizer->Add(CreateStdButtonSizer(this, ePreviewButton|eDefaultsButton|eCancelButton|eOkButton), 0, wxEXPAND);
#endif

   SetSizer(vSizer);

   wxSizer *paramSizer = new wxStaticBoxSizer(wxVERTICAL, sw, _("Effect Settings"));

   wxFlexGridSizer *gridSizer = new wxFlexGridSizer(4, 0, 0);
   gridSizer->AddGrowableCol(1);

   // Find the longest parameter name.
   int namew = 0;
   int w;
   int h;
   for (int i = 0; i < mAEffect->numParams; i++)
   {
      wxString text = mEffect->GetString(effGetParamName, i);
      if (text.Right(1) != wxT(':'))
      {
         text += wxT(':');
      }
      GetTextExtent(text, &w, &h);
      if (w > namew)
      {
         namew = w;
      }
   }

   GetTextExtent(wxT("HHHHHHHH"), &w, &h);

   for (int i = 0; i < mAEffect->numParams; i++)
   {
      mNames[i] = new wxStaticText(sw,
                                    wxID_ANY,
                                    wxEmptyString,
                                    wxDefaultPosition,
                                    wxSize(namew, -1),
                                    wxALIGN_RIGHT | wxST_NO_AUTORESIZE);
      gridSizer->Add(mNames[i], 0, wxALIGN_CENTER_VERTICAL | wxALIGN_RIGHT | wxALL, 5);

      mSliders[i] = new wxSlider(sw,
                                 ID_VST_SLIDERS + i,
                                 0,
                                 0,
                                 1000,
                                 wxDefaultPosition,
                                 wxSize(200, -1));
      gridSizer->Add(mSliders[i], 0, wxALIGN_CENTER_VERTICAL | wxEXPAND | wxALL, 5);

      mDisplays[i] = new wxStaticText(sw,
                                      wxID_ANY,
                                      wxEmptyString,
                                      wxDefaultPosition,
                                      wxSize(w, -1),
                                      wxALIGN_RIGHT | wxST_NO_AUTORESIZE);
      gridSizer->Add(mDisplays[i], 0, wxALIGN_CENTER_VERTICAL | wxALIGN_RIGHT | wxALL, 5);

      mLabels[i] = new wxStaticText(sw,
                                     wxID_ANY,
                                     wxEmptyString,
                                     wxDefaultPosition,
                                     wxSize(w, -1),
                                     wxALIGN_LEFT | wxST_NO_AUTORESIZE);
      gridSizer->Add(mLabels[i], 0, wxALIGN_CENTER_VERTICAL | wxALIGN_LEFT | wxALL, 5);
   }

   paramSizer->Add(gridSizer, 1, wxEXPAND | wxALL, 5);
   sw->SetSizer(paramSizer);

   Layout();
   Fit();
   SetSizeHints(GetSize());
   RefreshParameters();

   mSliders[0]->SetFocus();
}

wxSizer *VSTEffectDialog::BuildProgramBar()
{
   wxArrayString progs;

   // Some plugins, like Guitar Rig 5, only report 128 programs while they have hundreds.  While
   // I was able to come up with a hack in the Guitar Rig case to gather all of the program names,
   // it would not let me set a program outside of the first 128.
   for (int i = 0; i < mAEffect->numPrograms; i++)
   {
      progs.Add(mEffect->GetString(effGetProgramNameIndexed, i));
   }

   if (progs.GetCount() == 0)
   {
      progs.Add(_("None"));
   }

   wxString val;
   int progn = mEffect->callDispatcher(effGetProgram, 0, 0, NULL, 0.0);

   // An unset program is perfectly valid, do not force a default.
   if (progn >= 0 && progn < progs.GetCount())
   {
      val = progs[progn];
   }

   wxBoxSizer *hs = new wxBoxSizer(wxHORIZONTAL);

   wxStaticText *st = new wxStaticText(this, wxID_ANY, _("Presets:"));
   hs->Add(st, 0, wxALIGN_CENTER_VERTICAL | wxALL, 5);

   mProgram = new wxComboBox(this,
                             ID_VST_PROGRAM,
                             val,
                             wxDefaultPosition,
                             wxSize(200, -1),
                             progs);
   mProgram->SetName(_("Presets"));
   hs->Add(mProgram, 0, wxALIGN_CENTER_VERTICAL | wxALL, 5);

   wxButton *bt = new wxButton(this, ID_VST_LOAD, _("&Load"));
   hs->Add(bt, 0, wxALIGN_CENTER_VERTICAL | wxALL, 5);

   bt = new wxButton(this, ID_VST_SAVE, _("&Save"));
   hs->Add(bt, 0, wxALIGN_CENTER_VERTICAL | wxALL, 5);

   hs->AddStretchSpacer();

   bt = new wxButton(this, ID_VST_SETTINGS, _("S&ettings..."));
   hs->Add(bt, 0, wxALIGN_CENTER_VERTICAL | wxALIGN_LEFT | wxALL, 5);

   return hs;
}

void VSTEffectDialog::RefreshParameters(int skip)
{
   if (!mGui)
   {
      for (int i = 0; i < mAEffect->numParams; i++)
      {
         wxString text = mEffect->GetString(effGetParamName, i);
         text = text.Trim(true).Trim(false);

         wxString name = text;

         if (text.Right(1) != wxT(':'))
         {
            text += wxT(':');
         }
         mNames[i]->SetLabel(text);

         // For some parameters types like on/off, setting the slider value has
         // a side effect that causes it to only move when the parameter changes
         // from off to on.  However, this prevents changing the value using the
         // keyboard, so we skip the active slider if any.
         if (i != skip)
         {
            mSliders[i]->SetValue(mEffect->callGetParameter(i) * 1000);
         }
         name = text;

         text = mEffect->GetString(effGetParamDisplay, i);
         if (text.IsEmpty())
         {
            text.Printf(wxT("%.5g"),mEffect->callGetParameter(i));
         }
         mDisplays[i]->SetLabel(wxString::Format(wxT("%8s"), text.c_str()));
         name += wxT(' ') + text;

         text = mEffect->GetString(effGetParamDisplay, i);
         if (!text.IsEmpty())
         {
            text.Printf(wxT("%-8s"), mEffect->GetString(effGetParamLabel, i).c_str());
            mLabels[i]->SetLabel(wxString::Format(wxT("%8s"), text.c_str()));
            name += wxT(' ') + text;
         }

         mSliders[i]->SetName(name);
      }
   }
}

void VSTEffectDialog::OnUpdateDisplay(wxCommandEvent & evt)
{
   int i;

   Freeze();

   // Refresh the program list since some effects change the available programs based
   // on the users activity.
   mProgram->Clear();
   for (i = 0; i < mAEffect->numPrograms; i++)
   {
      mProgram->Append(mEffect->GetString(effGetProgramNameIndexed, i));
   }

   // The new list may not have the previously selected program or the user may have
   // changed it
   i = mEffect->callDispatcher(effGetProgram, 0, 0, NULL, 0.0);
   if (i >= 0)
   {
      mProgram->SetSelection(i);
   }

   Thaw();
}

void VSTEffectDialog::OnSizeWindow(wxCommandEvent & evt)
{
   if (!mContainer)
   {
      return;
   }

   mContainer->SetMinSize(evt.GetInt(), (int) evt.GetExtraLong());
   Fit();
   Layout();
}

void VSTEffectDialog::OnSlider(wxCommandEvent & evt)
{
   wxSlider *s = (wxSlider *) evt.GetEventObject();
   int i = s->GetId() - ID_VST_SLIDERS;

   mEffect->callSetParameter(i, s->GetValue() / 1000.0);

   RefreshParameters(i);
}

void VSTEffectDialog::OnProgram(wxCommandEvent & evt)
{
   mEffect->callDispatcher(effSetProgram, 0, evt.GetInt(), NULL, 0.0);
   RefreshParameters();
}

void VSTEffectDialog::OnProgramText(wxCommandEvent & WXUNUSED(evt))
{
   int i = mEffect->callDispatcher(effGetProgram, 0, 0, NULL, 0.0);

   // Bail if nothing is selected
   if (i < 0)
   {
      return;
   }

   wxString name = mProgram->GetValue();
   int ip = mProgram->GetInsertionPoint();

   // Limit the length of the string, max 24 + 1 for null terminator
   if (name.length() > 24)
   {
      name = name.substr(0, 24);
   }

   mEffect->SetString(effSetProgramName, name, i);

   // Some effects do not allow you to change the name and you can't always trust the
   // return value, so just get ask for the name again.
   name = mEffect->GetString(effGetProgramNameIndexed, i);

   mProgram->SetString(i, name);

   // On Windows, must reselect after doing a SetString()...at least that's
   // what seems to be required.
   mProgram->SetStringSelection(name);

   // Which also means we have to reposition the caret.
   if (ip >= 0)
   {
      mProgram->SetInsertionPoint(ip);
   }

   RefreshParameters();
}

//
// Load an "fxb", "fxp" or Audacuty "xml" file
//
// Based on work by Sven Giermann
//
void VSTEffectDialog::OnLoad(wxCommandEvent & WXUNUSED(evt))
{
   wxString path;

   // Ask the user for the real name
   path = FileSelector(_("Load VST Preset:"),
                     FileNames::DataDir(),
                     wxEmptyString,
                     wxT("xml"),
                       wxT("VST preset files (*.fxb; *.fxp; *.xml)|*.fxb;*.fxp;*.xml"),
                     wxFD_OPEN | wxRESIZE_BORDER,
                     this);

   // User canceled...
   if (path.IsEmpty())
   {
      return;
   }

   wxFileName fn(path);
   wxString ext = fn.GetExt();
   bool success = false;
   if (ext.CmpNoCase(wxT("fxb")) == 0)
   {
      success = LoadFXB(fn);
   }
   else if (ext.CmpNoCase(wxT("fxp")) == 0)
   {
      success = LoadFXP(fn);
   }
   else if (ext.CmpNoCase(wxT("xml")) == 0)
   {
      success = LoadXML(fn);
   }
   else
   {
      // This shouldn't happen, but complain anyway
      wxMessageBox(_("Unrecognized file extension."),
                      _("Error Loading VST Presets"),
                      wxOK | wxCENTRE,
                      this);

         return;
   }

   if (!success)
   {
      wxMessageBox(_("Unable to load presets file."),
                     _("Error Loading VST Presets"),
                     wxOK | wxCENTRE,
                     this);

      return;
   }

   RefreshParameters();

   return;
}

bool VSTEffectDialog::LoadFXB(const wxFileName & fn)
{
   bool ret = false;

   // Try to open the file...will be closed automatically when method returns
   wxFFile f(fn.GetFullPath(), wxT("rb"));
   if (!f.IsOpened())
   {
      return false;
   }

   // Allocate memory for the contents
   unsigned char *data = new unsigned char[f.Length()];
   if (!data)
   {
      wxMessageBox(_("Unable to allocate memory when loading presets file."),
                      _("Error Loading VST Presets"),
                      wxOK | wxCENTRE,
                      this);
      return false;
   }
   unsigned char *bptr = data;

   do
   {
      // Read in the whole file
      ssize_t len = f.Read((void *) bptr, f.Length());
      if (f.Error())
      {
         wxMessageBox(_("Unable to read presets file."),
                      _("Error Loading VST Presets"),
                      wxOK | wxCENTRE,
                      this);
         break;
      }

      // Most references to the data are via an "int" array
      int32_t *iptr = (int32_t *) bptr;

      // Verify that we have at least enough the header
      if (len < 156)
      {
         break;
      }

      // Verify that we probably have a FX file
      if (wxINT32_SWAP_ON_LE(iptr[0]) != CCONST('C', 'c', 'n', 'K'))
      {
         break;
      }

      // Ignore the size...sometimes it's there, other times it's zero

      // Get the version and verify
      int version = wxINT32_SWAP_ON_LE(iptr[3]);
      if (version != 1 && version != 2)
      {
         break;
      }

      // Ensure this program looks to belong to the current plugin
      if (wxINT32_SWAP_ON_LE(iptr[4]) != mAEffect->uniqueID)
      {
         break;
      }

      // Get the number of programs
      int numProgs = wxINT32_SWAP_ON_LE(iptr[6]);
      if (numProgs != mAEffect->numPrograms)
      {
         break;
      }

      // Get the current program index
      int curProg = 0;
      if (version == 2)
      {
         curProg = wxINT32_SWAP_ON_LE(iptr[7]);
         if (curProg < 0 || curProg >= numProgs)
         {
            break;
         }
      }

      // Is it a bank of programs?
      if (wxINT32_SWAP_ON_LE(iptr[2]) == CCONST('F', 'x', 'B', 'k'))
      {
         // Drop the header
         bptr += 156;
         len -= 156;

         unsigned char *tempPtr = bptr;
         ssize_t tempLen = len;

         // Validate all of the programs
         for (int i = 0; i < numProgs; i++)
         {
            if (!LoadFXProgram(&tempPtr, tempLen, i, true))
            {
               break;
            }
         }

         // They look okay, time to start changing things
         for (int i = 0; i < numProgs; i++)
         {
            ret = LoadFXProgram(&bptr, len, i, false);
         }
      }
      // Or maybe a bank chunk?
      else if (wxINT32_SWAP_ON_LE(iptr[2]) == CCONST('F', 'B', 'C', 'h'))
      {
         // Can't load programs chunks if the plugin doesn't support it
         if (!(mAEffect->flags & effFlagsProgramChunks))
         {
            break;
         }

         // Verify that we have enough to grab the chunk size
         if (len < 160)
         {
            break;
         }

         // Get the chunk size
         int size = wxINT32_SWAP_ON_LE(iptr[39]);

         // We finally know the full length of the program
         int proglen = 160 + size;

         // Verify that we have enough for the entire program
         if (len < proglen)
         {
            break;
         }

         // Set the entire bank in one shot
         mEffect->callDispatcher(effSetChunk, 0, size, &iptr[40], 0.0);

         // Success
         ret = true;
      }
      // Unrecognizable type
      else
      {
         break;
      }

      // Set the active program
      if (ret && version == 2)
      {
         mEffect->callDispatcher(effSetProgram, 0, curProg, NULL, 0.0);
         mProgram->SetSelection(curProg);
      }
   } while (false);

   // Get rid of the data
   delete [] data;

   return ret;
}

bool VSTEffectDialog::LoadFXP(const wxFileName & fn)
{
   bool ret = false;

   // Try to open the file...will be closed automatically when method returns
   wxFFile f(fn.GetFullPath(), wxT("rb"));
   if (!f.IsOpened())
   {
      return false;
   }

   // Allocate memory for the contents
   unsigned char *data = new unsigned char[f.Length()];
   if (!data)
   {
      wxMessageBox(_("Unable to allocate memory when loading presets file."),
                    _("Error Loading VST Presets"),
                     wxOK | wxCENTRE,
                     this);
      return false;
   }
   unsigned char *bptr = data;

   do
   {
      // Read in the whole file
      ssize_t len = f.Read((void *) bptr, f.Length());
      if (f.Error())
      {
         wxMessageBox(_("Unable to read presets file."),
                        _("Error Loading VST Presets"),
                        wxOK | wxCENTRE,
                        this);
         break;
      }

      // Get (or default) currently selected program
      int i = mProgram->GetCurrentSelection();
      if (i < 0)
      {
         i = 0;   // default to first program
      }

      // Go verify and set the program
      ret = LoadFXProgram(&bptr, len, i, false);
   } while (false);

   // Get rid of the data
   delete [] data;

   return ret;
}

bool VSTEffectDialog::LoadFXProgram(unsigned char **bptr, ssize_t & len, int index, bool dryrun)
{
   // Most references to the data are via an "int" array
   int32_t *iptr = (int32_t *) *bptr;

   // Verify that we have at least enough for a program without parameters
   if (len < 28)
   {
      return false;
   }

   // Verify that we probably have an FX file
   if (wxINT32_SWAP_ON_LE(iptr[0]) != CCONST('C', 'c', 'n', 'K'))
   {
      return false;
   }

   // Ignore the size...sometimes it's there, other times it's zero

   // Get the version and verify
#if defined(IS_THIS_AND_FXP_ARTIFICAL_LIMITATION)
   int version = wxINT32_SWAP_ON_LE(iptr[3]);
   if (version != 1)
   {
      return false;
   }
#endif

   // Ensure this program looks to belong to the current plugin
   if (wxINT32_SWAP_ON_LE(iptr[4]) != mAEffect->uniqueID)
   {
      return false;
   }

   // Get the number of parameters
   int numParams = wxINT32_SWAP_ON_LE(iptr[6]);
   if (numParams != mAEffect->numParams)
   {
      return false;
   }

   // At this point, we have to have enough to include the program name as well
   if (len < 56)
   {
      return false;
   }

   // Get the program name
   wxString progName(wxString::From8BitData((char *)&iptr[7]));

   // Might be a regular program
   if (wxINT32_SWAP_ON_LE(iptr[2]) == CCONST('F', 'x', 'C', 'k'))
   {
      // We finally know the full length of the program
      int proglen = 56 + (numParams * sizeof(float));

      // Verify that we have enough for all of the parameter values
      if (len < proglen)
      {
         return false;
      }

      // Validate all of the parameter values
      for (int i = 0; i < numParams; i++)
      {
         uint32_t ival = wxUINT32_SWAP_ON_LE(iptr[14 + i]);
         float val = *((float *) &ival);
         if (val < 0.0 || val > 1.0)
         {
            return false;
         }
      }
         
      // They look okay...time to start changing things
      if (!dryrun)
      {
         for (int i = 0; i < numParams; i++)
         {
            wxUint32 val = wxUINT32_SWAP_ON_LE(iptr[14 + i]);
            mEffect->callSetParameter(i, *((float *) &val));
         }
      }

      // Update in case we're loading an "FxBk" format bank file
      *bptr += proglen;
      len -= proglen;
   }
   // Maybe we have a program chunk
   else if (wxINT32_SWAP_ON_LE(iptr[2]) == CCONST('F', 'P', 'C', 'h'))
   {
      // Can't load programs chunks if the plugin doesn't support it
      if (!(mAEffect->flags & effFlagsProgramChunks))
      {
         return false;
      }

      // Verify that we have enough to grab the chunk size
      if (len < 60)
      {
         return false;
      }

      // Get the chunk size
      int size = wxINT32_SWAP_ON_LE(iptr[14]);

      // We finally know the full length of the program
      int proglen = 60 + size;

      // Verify that we have enough for the entire program
      if (len < proglen)
      {
         return false;
      }

      // Set the entire program in one shot
      if (!dryrun)
      {
         mEffect->callDispatcher(effSetChunk, 1, size, &iptr[15], 0.0);
      }

      // Update in case we're loading an "FxBk" format bank file
      *bptr += proglen;
      len -= proglen;
   }
   else
   {
      // Unknown type
      return false;
   }
   
   if (!dryrun)
   {
      mProgram->SetString(index, progName);
      mProgram->SetValue(progName);
      mEffect->SetString(effSetProgramName, wxString(progName), index);
   }

      return true;
}

bool VSTEffectDialog::LoadXML(const wxFileName & fn)
{
   // default to read as XML file
   // Load the program
   XMLFileReader reader;
   if (!reader.Parse(this, fn.GetFullPath()))
   {
      // Inform user of load failure
      wxMessageBox(reader.GetErrorStr(),
                   _("Error Loading VST Presets"),
                   wxOK | wxCENTRE,
                   this);
      return false;
   }

   return true;
}

void VSTEffectDialog::OnSave(wxCommandEvent & WXUNUSED(evt))
{
   int i = mProgram->GetCurrentSelection();
   wxString path;

   // Ask the user for the real name
   //
   // Passing a valid parent will cause some effects dialogs to malfunction
   // upon returning from the FileSelector().
   path = FileSelector(_("Save VST Preset As:"),
                       FileNames::DataDir(),
                       mProgram->GetValue(),
                       wxT("xml"),
                       wxT("Standard VST bank file (*.fxb)|*.fxb|Standard VST program file (*.fxp)|*.fxp|Audacity VST preset file (*.xml)|*.xml"),
                       wxFD_SAVE | wxFD_OVERWRITE_PROMPT | wxRESIZE_BORDER,
                       NULL);

   // User canceled...
   if (path.IsEmpty())
   {
      return;
   }

   wxFileName fn(path);
   wxString ext = fn.GetExt();
   if (ext.CmpNoCase(wxT("fxb")) == 0)
   {
      SaveFXB(fn);
   }
   else if (ext.CmpNoCase(wxT("fxp")) == 0)
   {
      SaveFXP(fn);
   }
   else if (ext.CmpNoCase(wxT("xml")) == 0)
   {
      SaveXML(fn);
   }
   else
   {
      // This shouldn't happen, but complain anyway
      wxMessageBox(_("Unrecognized file extension."),
                   _("Error Saving VST Presets"),
                   wxOK | wxCENTRE,
                   this);

      return;
   }
}

void VSTEffectDialog::SaveFXB(const wxFileName & fn)
{
   // Create/Open the file
   wxFFile f(fn.GetFullPath(), wxT("wb"));
   if (!f.IsOpened())
   {
      wxMessageBox(wxString::Format(_("Could not open file: \"%s\""), fn.GetFullPath().c_str()),
                   _("Error Saving VST Presets"),
                   wxOK | wxCENTRE,
                   this);
      return;
   }

   wxMemoryBuffer buf;
   wxInt32 subType;
   void *chunkPtr;
   int chunkSize;
   int dataSize = 148;
   wxInt32 tab[8];
   int curProg = mProgram->GetCurrentSelection();

   if (mAEffect->flags & effFlagsProgramChunks)
   {
      subType = CCONST('F', 'B', 'C', 'h');

      chunkSize = mEffect->callDispatcher(effGetChunk, 0, 0, &chunkPtr, 0.0);
      dataSize += 4 + chunkSize;
   }
   else
   {
      subType = CCONST('F', 'x', 'B', 'k');

      for (int i = 0; i < mAEffect->numPrograms; i++)
      {
         SaveFXProgram(buf, i);
      }

      dataSize += buf.GetDataLen();
   }

   tab[0] = wxINT32_SWAP_ON_LE(CCONST('C', 'c', 'n', 'K'));
   tab[1] = wxINT32_SWAP_ON_LE(dataSize);
   tab[2] = wxINT32_SWAP_ON_LE(subType);
   tab[3] = wxINT32_SWAP_ON_LE(curProg >= 0 ? 2 : 1);
   tab[4] = wxINT32_SWAP_ON_LE(mAEffect->uniqueID);
   tab[5] = wxINT32_SWAP_ON_LE(mAEffect->version);
   tab[6] = wxINT32_SWAP_ON_LE(mAEffect->numPrograms);
   tab[7] = wxINT32_SWAP_ON_LE(curProg >= 0 ? curProg : 0);

   f.Write(tab, sizeof(tab));
   if (!f.Error())
   {
      char padding[124];
      memset(padding, 0, sizeof(padding));
      f.Write(padding, sizeof(padding));

      if (!f.Error())
      {
         if (mAEffect->flags & effFlagsProgramChunks)
         {
            wxInt32 size = wxINT32_SWAP_ON_LE(chunkSize);
            f.Write(&size, sizeof(size));
            f.Write(chunkPtr, chunkSize);
         }
         else
         {
            f.Write(buf.GetData(), buf.GetDataLen());
         }
      }
   }

   if (f.Error())
   {
      wxMessageBox(wxString::Format(_("Error writing to file: \"%s\""), fn.GetFullPath().c_str()),
                   _("Error Saving VST Presets"),
                   wxOK | wxCENTRE,
                   this);
   }

   f.Close();

   return;
}

void VSTEffectDialog::SaveFXP(const wxFileName & fn)
{
   // Create/Open the file
   wxFFile f(fn.GetFullPath(), wxT("wb"));
   if (!f.IsOpened())
   {
      wxMessageBox(wxString::Format(_("Could not open file: \"%s\""), fn.GetFullPath().c_str()),
                   _("Error Saving VST Presets"),
                   wxOK | wxCENTRE,
                   this);
      return;
   }

   wxMemoryBuffer buf;

   int ndx = mEffect->callDispatcher(effGetProgram, 0, 0, NULL, 0.0);
   SaveFXProgram(buf, ndx);

   f.Write(buf.GetData(), buf.GetDataLen());
   if (f.Error())
   {
      wxMessageBox(wxString::Format(_("Error writing to file: \"%s\""), fn.GetFullPath().c_str()),
                   _("Error Saving VST Presets"),
                   wxOK | wxCENTRE,
                   this);
   }

   f.Close();

   return;
}

void VSTEffectDialog::SaveFXProgram(wxMemoryBuffer & buf, int index)
{
   wxInt32 subType;
   void *chunkPtr;
   int chunkSize;
   int dataSize = 48;
   char progName[28];
   wxInt32 tab[7];

   mEffect->callDispatcher(effGetProgramNameIndexed, index, 0, &progName, 0.0);
   progName[27] = '\0';
   chunkSize = strlen(progName);
   memset(&progName[chunkSize], 0, sizeof(progName) - chunkSize);

   if (mAEffect->flags & effFlagsProgramChunks)
   {
      subType = CCONST('F', 'P', 'C', 'h');

      chunkSize = mEffect->callDispatcher(effGetChunk, 1, 0, &chunkPtr, 0.0);
      dataSize += 4 + chunkSize;
   }
   else
   {
      subType = CCONST('F', 'x', 'C', 'k');

      dataSize += (mAEffect->numParams << 2);
   }

   tab[0] = wxINT32_SWAP_ON_LE(CCONST('C', 'c', 'n', 'K'));
   tab[1] = wxINT32_SWAP_ON_LE(dataSize);
   tab[2] = wxINT32_SWAP_ON_LE(subType);
   tab[3] = wxINT32_SWAP_ON_LE(1);
   tab[4] = wxINT32_SWAP_ON_LE(mAEffect->uniqueID);
   tab[5] = wxINT32_SWAP_ON_LE(mAEffect->version);
   tab[6] = wxINT32_SWAP_ON_LE(mAEffect->numParams);

   buf.AppendData(tab, sizeof(tab));
   buf.AppendData(progName, sizeof(progName));

   if (mAEffect->flags & effFlagsProgramChunks)
   {
      wxInt32 size = wxINT32_SWAP_ON_LE(chunkSize);
      buf.AppendData(&size, sizeof(size));
      buf.AppendData(chunkPtr, chunkSize);
   }
   else
   {
      for (int i = 0; i < mAEffect->numParams; i++)
      {
         float val = mEffect->callGetParameter(i);
         wxUint32 ival = wxUINT16_SWAP_ON_LE(*((wxUint32 *) &val));
         buf.AppendData(&ival, sizeof(ival));
      }
   }

   return;
}

void VSTEffectDialog::SaveXML(const wxFileName & fn)
{
   XMLFileWriter xmlFile;

   // Create/Open the file
   xmlFile.Open(fn.GetFullPath(), wxT("wb"));

   xmlFile.StartTag(wxT("vstprogrampersistence"));
   xmlFile.WriteAttr(wxT("version"), wxT("1"));

   xmlFile.StartTag(wxT("effect"));
   xmlFile.WriteAttr(wxT("name"), mEffect->GetName());
   xmlFile.WriteAttr(wxT("version"), mEffect->callDispatcher(effGetVendorVersion, 0, 0, NULL, 0.0));

   xmlFile.StartTag(wxT("program"));
   xmlFile.WriteAttr(wxT("name"), mProgram->GetValue());

   int clen = 0;
   if (mAEffect->flags & effFlagsProgramChunks)
   {
      void *chunk = NULL;

      clen = (int) mEffect->callDispatcher(effGetChunk, 1, 0, &chunk, 0.0);
      if (clen != 0)
      {
         xmlFile.StartTag(wxT("chunk"));
         xmlFile.WriteSubTree(VSTEffect::b64encode(chunk, clen) + wxT('\n'));
         xmlFile.EndTag(wxT("chunk"));
      }
   }

   if (clen == 0)
   {
      for (int i = 0; i < mAEffect->numParams; i++)
      {
         xmlFile.StartTag(wxT("param"));

         xmlFile.WriteAttr(wxT("index"), i);
         xmlFile.WriteAttr(wxT("name"),
                           mEffect->GetString(effGetParamName, i));
         xmlFile.WriteAttr(wxT("value"),
                           wxString::Format(wxT("%f"),
                           mEffect->callGetParameter(i)));

         xmlFile.EndTag(wxT("param"));
      }
   }

   xmlFile.EndTag(wxT("program"));

   xmlFile.EndTag(wxT("effect"));

   xmlFile.EndTag(wxT("vstprogrampersistence"));

   // Close the file
   xmlFile.Close();

   return;
}


void VSTEffectDialog::OnSettings(wxCommandEvent & WXUNUSED(evt))
{
   VSTEffectSettingsDialog dlg(this, mEffect->mHost);
   if (dlg.ShowModal())
   {
      // Call Startup() to reinitialize configuration settings
      mEffect->Startup();
   }
}

void VSTEffectDialog::OnClose(wxCloseEvent & evt)
{
#if defined(EXPERIMENTAL_REALTIME_EFFECTS)

#if defined(__WXMAC__)
   Destroy();
#else
   Show(false);
   evt.Veto();
#endif

#else
   EndModal(false);
#endif
}

#if defined(EXPERIMENTAL_REALTIME_EFFECTS)
void VSTEffectDialog::OnApply(wxCommandEvent & WXUNUSED(evt))
{
#if defined(__WXMAC__)
   Close();
#else
   Show(false);
#endif

   mEffect->mHost->Apply();
}
#else
void VSTEffectDialog::OnPreview(wxCommandEvent & WXUNUSED(evt))
{
   mEffect->mHost->Preview();
}

void VSTEffectDialog::OnOk(wxCommandEvent & WXUNUSED(evt))
{
// In wxGTK, Show(false) calls EndModal, which produces an assertion in debug builds
#if !defined(__WXGTK__)
   // Hide the dialog before closing the effect to prevent a brief empty dialog
   Show(false);
#endif

   if (mGui)
   {
//      mEffect->PowerOff();
//      mEffect->NeedEditIdle(false);
//      mEffect->callDispatcher(effEditClose, 0, 0, NULL, 0.0);
   }

   EndModal(true);
}

void VSTEffectDialog::OnCancel(wxCommandEvent & WXUNUSED(evt))
{
// In wxGTK, Show(false) calls EndModal, which produces an assertion in debug builds
#if !defined(__WXGTK__)
   // Hide the dialog before closing the effect to prevent a brief empty dialog
   Show(false);
#endif

   if (mGui)
   {
//      mEffect->PowerOff();
//      mEffect->NeedEditIdle(false);
//      mEffect->callDispatcher(effEditClose, 0, 0, NULL, 0.0);
   }

   EndModal(false);
}
#endif

void VSTEffectDialog::OnDefaults(wxCommandEvent & WXUNUSED(evt))
{
   mEffect->LoadParameters(wxT("Default"));
   RefreshParameters();
}

bool VSTEffectDialog::HandleXMLTag(const wxChar *tag, const wxChar **attrs)
{
   if (wxStrcmp(tag, wxT("vstprogrampersistence")) == 0)
   {
      while (*attrs)
      {
         const wxChar *attr = *attrs++;
         const wxChar *value = *attrs++;

         if (!value)
         {
            break;
         }

         const wxString strValue = value;

         if (wxStrcmp(attr, wxT("version")) == 0)
         {
            if (!XMLValueChecker::IsGoodInt(strValue))
            {
               return false;
            }
            // Nothing to do with it for now
         }
         else
         {
            return false;
         }
      }

      return true;
   }

   if (wxStrcmp(tag, wxT("effect")) == 0)
   {
      while (*attrs)
      {
         const wxChar *attr = *attrs++;
         const wxChar *value = *attrs++;

         if (!value)
         {
            break;
         }

         const wxString strValue = value;

         if (wxStrcmp(attr, wxT("name")) == 0)
         {
            if (!XMLValueChecker::IsGoodString(strValue))
            {
               return false;
            }

            if (value != mEffect->GetName())
            {
               wxString msg;
               msg.Printf(_("This parameter file was saved from %s.  Continue?"), value);
               int result = wxMessageBox(msg, wxT("Confirm"), wxYES_NO, this);
               if (result == wxNO)
               {
                  return false;
               }
            }
         }
         else if (wxStrcmp(attr, wxT("version")) == 0)
         {
            if (!XMLValueChecker::IsGoodInt(strValue))
            {
               return false;
            }
            // Nothing to do with it for now
         }
         else
         {
            return false;
         }
      }

      return true;
   }

   if (wxStrcmp(tag, wxT("program")) == 0)
   {
      while (*attrs)
      {
         const wxChar *attr = *attrs++;
         const wxChar *value = *attrs++;

         if (!value)
         {
            break;
         }

         const wxString strValue = value;

         if (wxStrcmp(attr, wxT("name")) == 0)
         {
            if (!XMLValueChecker::IsGoodString(strValue))
            {
               return false;
            }

            if (strValue.length() > 24)
            {
               return false;
            }

            int ndx = mProgram->GetCurrentSelection();
            if (ndx == wxNOT_FOUND)
            {
               ndx = 0;
            }

            mProgram->SetString(ndx, strValue);
            mProgram->SetValue(strValue);

            mEffect->SetString(effSetProgramName, strValue, ndx);
         }
         else
         {
            return false;
         }
      }

      mInChunk = false;

      return true;
   }

   if (wxStrcmp(tag, wxT("param")) == 0)
   {
      long ndx = -1;
      double val = -1.0;
      while (*attrs)
      {
         const wxChar *attr = *attrs++;
         const wxChar *value = *attrs++;

         if (!value)
         {
            break;
         }

         const wxString strValue = value;

         if (wxStrcmp(attr, wxT("index")) == 0)
         {
            if (!XMLValueChecker::IsGoodInt(strValue) || !strValue.ToLong(&ndx))
            {
               return false;
            }

            if (ndx < 0 || ndx >= mAEffect->numParams)
            {
               // Could be a different version of the effect...probably should
               // tell the user
               return false;
            }
         }
         else if (wxStrcmp(attr, wxT("name")) == 0)
         {
            if (!XMLValueChecker::IsGoodString(strValue))
            {
               return false;
            }
            // Nothing to do with it for now
         }
         else if (wxStrcmp(attr, wxT("value")) == 0)
         {
            if (!XMLValueChecker::IsGoodInt(strValue) ||
               !Internat::CompatibleToDouble(strValue, &val))
            {
               return false;
            }

            if (val < 0.0 || val > 1.0)
            {
               return false;
            }
         }
      }

      if (ndx == -1 || val == -1.0)
      {
         return false;
      }

      mEffect->callSetParameter(ndx, val);

      return true;
   }

   if (wxStrcmp(tag, wxT("chunk")) == 0)
   {
      mInChunk = true;
      return true;
   }

   return false;
}

void VSTEffectDialog::HandleXMLEndTag(const wxChar *tag)
{
   if (wxStrcmp(tag, wxT("chunk")) == 0)
   {
      if (mChunk.length())
      {
         char *buf = new char[mChunk.length() / 4 * 3];

         int len = VSTEffect::b64decode(mChunk, buf);
         if (len)
         {
            mEffect->callDispatcher(effSetChunk, 1, len, buf, 0.0);
         }

         delete [] buf;
         mChunk.clear();
      }
      mInChunk = false;
   }
}

void VSTEffectDialog::HandleXMLContent(const wxString & content)
{
   if (mInChunk)
   {
      mChunk += wxString(content).Trim(true).Trim(false);
   }
}

XMLTagHandler *VSTEffectDialog::HandleXMLChild(const wxChar *tag)
{
   if (wxStrcmp(tag, wxT("vstprogrampersistence")) == 0)
   {
      return this;
   }

   if (wxStrcmp(tag, wxT("effect")) == 0)
   {
      return this;
   }

   if (wxStrcmp(tag, wxT("program")) == 0)
   {
      return this;
   }

   if (wxStrcmp(tag, wxT("param")) == 0)
   {
      return this;
   }

   if (wxStrcmp(tag, wxT("chunk")) == 0)
   {
      return this;
   }

   return NULL;
}

///////////////////////////////////////////////////////////////////////////////
//
// VSTEffectTimer
//
///////////////////////////////////////////////////////////////////////////////

class VSTEffectTimer : public wxTimer
{
public:
   VSTEffectTimer(VSTEffect *effect)
   :  wxTimer(),
      mEffect(effect)
   {
   }

   ~VSTEffectTimer()
   {
   }

   void Notify()
   {
      mEffect->OnTimer();
   }

private:
   VSTEffect *mEffect;
};

///////////////////////////////////////////////////////////////////////////////
//
// VSTEffect
//
///////////////////////////////////////////////////////////////////////////////

typedef AEffect *(*vstPluginMain)(audioMasterCallback audioMaster);

intptr_t VSTEffect::AudioMaster(AEffect * effect,
                            int32_t opcode,
                            int32_t index,
                            intptr_t value,
                            void * ptr,
                            float opt)
{

   VSTEffect *vst = (effect ? (VSTEffect *) effect->user : NULL);

   // Handles operations during initialization...before VSTEffect has had a
   // chance to set its instance pointer.
   switch (opcode)
   {
      case audioMasterVersion:
         return (intptr_t) 2400;

      case audioMasterCurrentId:
         return (intptr_t) audacityVSTID;

      case audioMasterGetVendorString:
         strcpy((char *) ptr, "Audacity Team");    // Do not translate, max 64 + 1 for null terminator
         return 1;

      case audioMasterGetProductString:
         strcpy((char *) ptr, "Audacity");         // Do not translate, max 64 + 1 for null terminator
         return 1;

      case audioMasterGetVendorVersion:
         return (intptr_t) (AUDACITY_VERSION << 24 |
                            AUDACITY_RELEASE << 16 |
                            AUDACITY_REVISION << 8 |
                            AUDACITY_MODLEVEL);

      // Some (older) effects depend on an effIdle call when requested.  An
      // example is the Antress Modern plugins which uses the call to update
      // the editors display when the program (preset) changes.
      case audioMasterNeedIdle:
         if (vst)
         {
            vst->NeedIdle();
            return 1;
         }
         return 0;

      // We would normally get this if the effect editor is dipslayed and something "major"
      // has changed (like a program change) instead of multiple automation calls.
      // Since we don't do anything with the parameters while the editor is displayed,
      // there's no need for us to do anything.
      case audioMasterUpdateDisplay:
         if (vst)
         {
            vst->UpdateDisplay();
            return 1;
         }
         return 0;

      // Return the current time info.
      case audioMasterGetTime:
         if (vst)
         {
            return (intptr_t) vst->GetTimeInfo();
         }
         return 0;

      // Inputs, outputs, or initial delay has changed...all we care about is initial delay.
      case audioMasterIOChanged:
         if (vst)
         {
            vst->SetBufferDelay(effect->initialDelay);
            return 1;
         }
         return 0;

      case audioMasterGetSampleRate:
         if (vst)
         {
            return (intptr_t) vst->GetSampleRate();
         }
         return 0;

      case audioMasterIdle:
         wxYieldIfNeeded();
         return 1;

      case audioMasterGetCurrentProcessLevel:
         if (vst)
         {
            return vst->GetProcessLevel();
         }
         return 0;

      case audioMasterGetLanguage:
         return kVstLangEnglish;

      // We always replace, never accumulate
      case audioMasterWillReplaceOrAccumulate:
         return 1;

      // Resize the window to accommodate the effect size
      case audioMasterSizeWindow:
         if (vst)
         {
            vst->SizeWindow(index, value);
         }
         return 1;

      case audioMasterCanDo:
      {
         char *s = (char *) ptr;
         if (strcmp(s, "acceptIOChanges") == 0 ||
            strcmp(s, "sizeWindow") == 0)
         {
            return 1;
         }

#if defined(VST_DEBUG)
#if defined(__WXMSW__)
         wxLogDebug(wxT("VST canDo: %s"), wxString::FromAscii((char *)ptr).c_str());
#else
         wxPrintf(wxT("VST canDo: %s\n"), wxString::FromAscii((char *)ptr).c_str());
#endif
#endif

         return 0;
      }

#if defined(EXPERIMENTAL_REALTIME_EFFECTS)
      case audioMasterAutomate:
         if (vst)
            vst->Automate(index, opt);
         return 0;

#else
      // These are not needed since we don't need the parameter values until after the editor
      // has already been closed.  If we did realtime effects, then we'd need these.
      case audioMasterBeginEdit:
      case audioMasterEndEdit:
      case audioMasterAutomate:
#endif
      // We're always connected (sort of)
      case audioMasterPinConnected:

      // We don't do MIDI yet
      case audioMasterWantMidi:
      case audioMasterProcessEvents:

         // Don't need to see any messages about these
         return 0;
   }

#if defined(VST_DEBUG)
#if defined(__WXMSW__)
   wxLogDebug(wxT("vst: %p opcode: %d index: %d value: %d ptr: %p opt: %f user: %p"),
              effect, opcode, index, value, ptr, opt, vst);
#else
   wxPrintf(wxT("vst: %p opcode: %d index: %d value: %d ptr: %p opt: %f user: %p\n"),
            effect, opcode, index, value, ptr, opt, vst);
#endif
#endif

   return 0;
}

VSTEffect::VSTEffect(const wxString & path, VSTEffect *master)
:  mPath(path),
   mMaster(master)
{
   mHost = NULL;
   mModule = NULL;
   mAEffect = NULL;
   mDlg = NULL;
   mTimer = new VSTEffectTimer(this);
   mTimerGuard = 0;

   mInteractive = false;
   mAudioIns = 0;
   mAudioOuts = 0;
   mMidiIns = 0;
   mMidiOuts = 0;
   mBlockSize = 0;
   mBufferDelay = 0;
   mProcessLevel = 1;         // in GUI thread
   mHasPower = false;
   mWantsIdle = false;
   mWantsEditIdle = false;
   mUserBlockSize = 8192;
   mBlockSize = mUserBlockSize;
   mUseBufferDelay = true;
   mReady = false;

   memset(&mTimeInfo, 0, sizeof(mTimeInfo));
   mTimeInfo.samplePos = 0.0;
   mTimeInfo.sampleRate = 44100.0;  // this is a bogus value, but it's only for the display
   mTimeInfo.nanoSeconds = wxGetLocalTimeMillis().ToDouble();
   mTimeInfo.tempo = 120.0;
   mTimeInfo.timeSigNumerator = 4;
   mTimeInfo.timeSigDenominator = 4;
   mTimeInfo.flags = kVstTempoValid | kVstNanosValid;

   // If we're a slave then go ahead a load immediately
   if (mMaster)
   {
      Load();
   }
}

VSTEffect::~VSTEffect()
{
   Unload();
}

//
// EffectClientInterface Implementation
//
void VSTEffect::SetHost(EffectHostInterface *host)
{
   mHost = host;
   Startup();
}

bool VSTEffect::Startup()
{
   if (!mAEffect)
   {
      Load();
   }

   if (!mAEffect)
   {
      return false;
   }

   // mHost will be null when running in the subprocess
   if (mHost)
   {
      mHost->GetSharedConfig(wxT("Settings"), wxT("BufferSize"), mUserBlockSize, 8192);
      mHost->GetSharedConfig(wxT("Settings"), wxT("UseBufferDelay"), mUseBufferDelay, true);

      mBlockSize = mUserBlockSize;

      bool haveDefaults;
      mHost->GetPrivateConfig(wxT("Default"), wxT("Initialized"), haveDefaults, false);
      if (!haveDefaults)
      {
         SaveParameters(wxT("Default"));
         mHost->SetPrivateConfig(wxT("Default"), wxT("Initialized"), true);
      }

      LoadParameters(wxT("Current"));
   }

   return true;
}

bool VSTEffect::Shutdown()
{
   SaveParameters(wxT("Current"));

   return true;
}

EffectType VSTEffect::GetType()
{
   if (mAudioIns == 0 && mMidiIns == 0)
   {
      return EffectTypeGenerate;
   }

   if (mAudioOuts == 0 && mMidiOuts == 0)
   {
      return EffectTypeAnalyze;
   }

   return EffectTypeProcess;
}

wxString VSTEffect::GetID()
{
   return wxString(wxT("VST_") + GetVendor() + wxT("_") + GetName() + wxT("_") + GetVersion());
}

wxString VSTEffect::GetPath()
{
   return mPath;
}

wxString VSTEffect::GetName()
{
   return mName;
}

wxString VSTEffect::GetVendor()
{
   return mVendor;
}

wxString VSTEffect::GetVersion()
{
   wxString version;

   bool skipping = true;
   for (int i = 0, s = 0; i < 4; i++, s += 8)
   {
      int dig = (mVersion >> s) & 0xff;
      if (dig != 0 || !skipping)
      {
         version += !skipping ? wxT(".") : wxT("");
         version += wxString::Format(wxT("%d"), dig);
         skipping = false;
      }
   }

   return version;
}

wxString VSTEffect::GetDescription()
{
   // VST does have a product string opcode and sum effects return a short
   // description, but most do not or they just return the name again.  So,
   // try to provide some sort of useful information.
   mDescription = _("Audio In: ") +
                  wxString::Format(wxT("%d"), mAudioIns),
                  _(", Audio Out: ") +
                  wxString::Format(wxT("%d"), mAudioOuts);

   return mDescription;
}

wxString VSTEffect::GetFamily()
{
   return VSTPLUGINTYPE;
}

bool VSTEffect::IsInteractive()
   {
   return mInteractive;
}

bool VSTEffect::IsDefault()
{
   return false;
}

bool VSTEffect::IsLegacy()
{
   return false;
}

bool VSTEffect::IsRealtimeCapable()
{
   return true;
}

int VSTEffect::GetAudioInCount()
{
   return mAudioIns;
}

int VSTEffect::GetAudioOutCount()
{
   return mAudioOuts;
}

int VSTEffect::GetMidiInCount()
{
   return mMidiIns;
}

int VSTEffect::GetMidiOutCount()
{
   return mMidiOuts;
}

sampleCount VSTEffect::GetBlockSize(sampleCount maxBlockSize)
{
   sampleCount prevSize = mBlockSize;

   if (mUserBlockSize > maxBlockSize)
   {
      mBlockSize = maxBlockSize;
   }
   else
   {
      mBlockSize = mUserBlockSize;
   }

   return mBlockSize;
}

void VSTEffect::SetSampleRate(sampleCount rate)
{
   mSampleRate = (float) rate;
}

sampleCount VSTEffect::GetLatency()
{
   if (mUseBufferDelay)
   {
      // ??? Threading issue ???
      sampleCount delay = mBufferDelay;
      mBufferDelay = 0;
      return delay;
   }

   return 0;
}

sampleCount VSTEffect::GetTailSize()
{
   return 0;
}

bool VSTEffect::IsReady()
{
   return mReady;
}

bool VSTEffect::ProcessInitialize()
{
   // Initialize time info
   memset(&mTimeInfo, 0, sizeof(mTimeInfo));
   mTimeInfo.sampleRate = mSampleRate;
   mTimeInfo.nanoSeconds = wxGetLocalTimeMillis().ToDouble();
   mTimeInfo.tempo = 120.0;
   mTimeInfo.timeSigNumerator = 4;
   mTimeInfo.timeSigDenominator = 4;
   mTimeInfo.flags = kVstTempoValid | kVstNanosValid | kVstTransportPlaying;

   // Set processing parameters...power must be off for this
   callDispatcher(effSetSampleRate, 0, 0, NULL, mSampleRate);
   callDispatcher(effSetBlockSize, 0, mBlockSize, NULL, 0.0);

   // Turn on the power
   PowerOn();

   // Set the initial buffer delay
   SetBufferDelay(mAEffect->initialDelay);

   mReady = true;

   return true;
}

bool VSTEffect::ProcessFinalize()
{
   mReady = false;

   PowerOff();

   return true;
}

sampleCount VSTEffect::ProcessBlock(float **inbuf, float **outbuf, sampleCount size)
{
   // Go let the plugin moleste the samples
   callProcessReplacing(inbuf, outbuf, size);
   mTimeInfo.samplePos += ((double) size / mTimeInfo.sampleRate);

   return size;
}

bool VSTEffect::RealtimeInitialize()
{
   // This is really just a dummy value and one to make the dialog happy since
   // all processing is handled by slaves.
   SetSampleRate(44100);

   return ProcessInitialize();
}

bool VSTEffect::RealtimeAddProcessor(int numChannels, float sampleRate)
{
   VSTEffect *slave = new VSTEffect(mPath, this);
   mSlaves.Add(slave);

   slave->SetSampleRate(sampleRate);

   return ProcessInitialize();
}

bool VSTEffect::RealtimeFinalize()
{
   for (size_t i = 0, cnt = mSlaves.GetCount(); i < cnt; i++)
   {
      delete mSlaves[i];
   }
   mSlaves.Clear();

   return ProcessFinalize();
}

bool VSTEffect::RealtimeSuspend()
{
   PowerOff();

   return true;
}

bool VSTEffect::RealtimeResume()
{
   PowerOn();

   return true;
}

sampleCount VSTEffect::RealtimeProcess(int index, float **inbuf, float **outbuf, sampleCount size)
{
   if (index < 0 || index >= mSlaves.GetCount())
   {
      return 0;
   }

   return mSlaves[index]->ProcessBlock(inbuf, outbuf, size);
}

//
// Some history...
//
// Before we ran into the Antress plugin problem with buffer size limitations,
// (see below) we just had a plain old effect loop...get the input samples, pass
// them to the effect, save the output samples.
//
// But, the hack I put in to limit the buffer size to only 8k (normally 512k or so)
// severely impacted performance.  So, Michael C. added some intermediate buffering
// that sped things up quite a bit and this is how things have worked for quite a
// while.  It still didn't get the performance back to the pre-hack stage, but it
// was a definite benefit.
//
// History over...
//
// I've recently (May 2014) tried newer versions of the Antress effects and they
// no longer seem to have a problem with buffer size.  So, I've made a bit of a
// compromise...I've made the buffer size user configurable.  Should have done this
// from the beginning.  I've left the default 8k, just in case, but now the user
// can set the buffering based on their specific setup and needs.
//
// And at the same time I added buffer delay compensation, which allows Audacity
// to account for latency introduced by some effects.  This is based on information
// provided by the effect, so it will not work with all effects since they don't
// all provide the information (kn0ck0ut is one).
//
bool VSTEffect::ShowInterface(void *parent)
{
//   mProcessLevel = 1;      // in GUI thread

   // Set some defaults since some VSTs need them...these will be reset when
   // normal or realtime processing begins
   if (!IsReady())
   {
      mSampleRate = 44100;
      mBlockSize = 8192;
      ProcessInitialize();
   }

   // I can't believe we haven't run into this before, but a terrible assumption has
   // been made all along...effects do NOT have to provide textual parameters.  Examples
   // of effects that do not support parameters are some from BBE Sound.  These effects
   // are NOT broken.  They just weren't written to support textual parameters.
   long gui = (gPrefs->Read(wxT("/VST/GUI"), (long) true) != 0);
   if (!gui && mAEffect->numParams == 0)
   {
#if defined(__WXGTK__)
      wxMessageBox(_("This effect does not support a textual interface. At this time, you may not use this effect on Linux."),
                   _("VST Effect"));
#else
      wxMessageBox(_("This effect does not support a textual interface.  Falling back to graphical display."),
                   _("VST Effect"));
#endif
   }

   if (!mDlg)
   {
      mDlg = new VSTEffectDialog((wxWindow *) parent, mName, this, mAEffect);
      mDlg->CentreOnParent();
   }

#if defined(EXPERIMENTAL_REALTIME_EFFECTS)
   mDlg->Show(!mDlg->IsShown());

   return true;
#else
   mDlg->ShowModal();
   bool ret = mDlg->GetReturnCode() != 0;
   mDlg->Destroy();
   mDlg = NULL;

   return ret;
#endif
}

void VSTEffect::InterfaceClosed()
{
   mDlg = NULL;
}

bool VSTEffect::Load()
{
   vstPluginMain pluginMain;
   bool success = false;

   mModule = NULL;
   mAEffect = NULL;

#if defined(__WXMAC__)
   // Start clean
   mBundleRef = NULL;

   // Don't really know what this should be initialize to
   mResource = -1;

   // Convert the path to a CFSTring
   wxMacCFStringHolder path(mPath);

   // Convert the path to a URL
   CFURLRef urlRef =
      CFURLCreateWithFileSystemPath(kCFAllocatorDefault,
                                    path,
                                    kCFURLPOSIXPathStyle,
                                    true);
   if (urlRef == NULL)
   {
      return false;
   }

   // Create the bundle using the URL
   CFBundleRef bundleRef = CFBundleCreate(kCFAllocatorDefault, urlRef);

   // Done with the URL
   CFRelease(urlRef);

   // Bail if the bundle wasn't created
   if (bundleRef == NULL) 
   {
      return false;
   }

   // Retrieve a reference to the executable
   CFURLRef exeRef = CFBundleCopyExecutableURL(bundleRef);
   if (exeRef == NULL)
   {
      CFRelease(bundleRef);
      return false;
   }

   // Convert back to path
   UInt8 exePath[PLATFORM_MAX_PATH];
   Boolean good = CFURLGetFileSystemRepresentation(exeRef, true, exePath, sizeof(exePath));

   // Done with the executable reference
   CFRelease(exeRef);

   // Bail if we couldn't resolve the executable path
   if (good == FALSE)
   {
      CFRelease(bundleRef);
      return false;
   }

   // Attempt to open it
   mModule = dlopen((char *) exePath, RTLD_NOW | RTLD_LOCAL);
   if (mModule == NULL)
   {
      CFRelease(bundleRef);
      return false;
   }

   // Try to locate the new plugin entry point
   pluginMain = (vstPluginMain) dlsym(mModule, "VSTPluginMain");

   // If not found, try finding the old entry point
   if (pluginMain == NULL)
   {
      pluginMain = (vstPluginMain) dlsym(mModule, "main_macho");
   }

   // Must not be a VST plugin
   if (pluginMain == NULL)
   {
      dlclose(mModule);
      mModule = NULL;
      CFRelease(bundleRef);
      return false;
   }

   // Need to keep the bundle reference around so we can map the
   // resources.
   mBundleRef = bundleRef;

   // Open the resource map ... some plugins (like GRM Tools) need this.
   mResource = (int) CFBundleOpenBundleResourceMap(bundleRef);

#elif defined(__WXMSW__)

   {
      wxLogNull nolog;

      // Try to load the library
      wxDynamicLibrary *lib = new wxDynamicLibrary(mPath);
      if (!lib) 
      {
         return false;
      }

      // Bail if it wasn't successful
      if (!lib->IsLoaded())
      {
         delete lib;
         return false;
      }

      // Try to find the entry point, while suppressing error messages
      pluginMain = (vstPluginMain) lib->GetSymbol(wxT("VSTPluginMain"));
      if (pluginMain == NULL)
      {
         pluginMain = (vstPluginMain) lib->GetSymbol(wxT("main"));
         if (pluginMain == NULL)
         {
            delete lib;
            return false;
         }
      }

      // Save the library reference
      mModule = lib;
   }

#else

   // Attempt to load it
   //
   // Spent a few days trying to figure out why some VSTs where running okay and
   // others were hit or miss.  The cause was that we export all of Audacity's
   // symbols and some of the loaded libraries were picking up Audacity's and 
   // not their own.
   //
   // So far, I've only seen this issue on Linux, but we might just be getting
   // lucky on the Mac and Windows.  The sooner we stop exporting everything
   // the better.
   //
   // To get around the problem, I just added the RTLD_DEEPBIND flag to the load
   // and that "basically" puts Audacity last when the loader needs to resolve
   // symbols.
   //
   // Once we define a proper external API, the flags can be removed.
   void *lib = dlopen((const char *)wxString(mPath).ToUTF8(), RTLD_NOW | RTLD_LOCAL | RTLD_DEEPBIND);
   if (!lib) 
   {
      return false;
   }

   // Try to find the entry point, while suppressing error messages
   pluginMain = (vstPluginMain) dlsym(lib, "VSTPluginMain");
   if (pluginMain == NULL)
   {
      pluginMain = (vstPluginMain) dlsym(lib, "main");
      if (pluginMain == NULL)
      {
         dlclose(lib);
         return false;
      }
   }

   // Save the library reference
   mModule = lib;

#endif

   // Initialize the plugin
   try
   {
      mAEffect = pluginMain(VSTEffect::AudioMaster);
   }
   catch (...)
   {
      wxLogMessage(_("VST plugin initialization failed\n"));
      mAEffect = NULL;
   }

   // Was it successful?
   if (mAEffect)
   {
      // Save a reference to ourselves
      //
      // Note:  Some hosts use "user" and some use "ptr2/resvd2".  It might
      //        be worthwhile to check if user is NULL before using it and
      //        then falling back to "ptr2/resvd2".
      mAEffect->user = this;

      // Give the plugin an initial sample rate and blocksize
      callDispatcher(effSetSampleRate, 0, 0, NULL, 48000.0);
      callDispatcher(effSetBlockSize, 0, 512, NULL, 0);

      // Ask the plugin to identify itself...might be needed for older plugins
      callDispatcher(effIdentify, 0, 0, NULL, 0);

      // Open the plugin
      callDispatcher(effOpen, 0, 0, NULL, 0.0);

      // Set it again in case plugin ignored it before the effOpen
      callDispatcher(effSetSampleRate, 0, 0, NULL, 48000.0);
      callDispatcher(effSetBlockSize, 0, 512, NULL, 0);

      // Ensure that it looks like a plugin and can deal with ProcessReplacing
      // calls.  Also exclude synths for now.
      if (mAEffect->magic == kEffectMagic &&
         !(mAEffect->flags & effFlagsIsSynth) &&
         mAEffect->flags & effFlagsCanReplacing)
      {
         mName = GetString(effGetEffectName);
         if (mName.length() == 0)
         {
            mName = GetString(effGetProductString);
            if (mName.length() == 0)
            {
               wxFileName f(mPath);
               mName = f.GetName();
            }
         }
         mVendor = GetString(effGetVendorString);
         mVersion = wxINT32_SWAP_ON_LE(callDispatcher(effGetVendorVersion, 0, 0, NULL, 0));
         if (mVersion == 0)
         {
            mVersion = wxINT32_SWAP_ON_LE(mAEffect->version);
         }

         if (mAEffect->flags & effFlagsHasEditor || mAEffect->numParams != 0)
         {
            mInteractive = true;
         }

         mAudioIns = mAEffect->numInputs;
         mAudioOuts = mAEffect->numOutputs;

         mMidiIns = 0;
         mMidiOuts = 0;

         // Pretty confident that we're good to go
         success = true;
      }
   }

   if (!success)
   {
      Unload();
   }

   return success;
}

void VSTEffect::Unload()
{
   if (mTimer)
   {
      mTimer->Stop();
      delete mTimer;
      mTimer = NULL;
   }

   if (mAEffect)
   {
      // Turn the power off
      PowerOff();

      // Finally, close the plugin
      callDispatcher(effClose, 0, 0, NULL, 0.0);
   }

   if (mModule)
   {
#if defined(__WXMAC__)

      if (mResource != -1)
      {
         CFBundleCloseBundleResourceMap((CFBundleRef) mBundleRef, mResource);
         mResource = -1;
      }

      if (mBundleRef != NULL)
      {
         CFRelease((CFBundleRef) mBundleRef);
         mBundleRef = NULL;
      }

      dlclose(mModule);

#elif defined(__WXMSW__)

      delete (wxDynamicLibrary *) mModule;

#else

      dlclose(mModule);

#endif

      mModule = NULL;
      mAEffect = NULL;
   }
}

void VSTEffect::LoadParameters(const wxString & group)
{
   wxString value;

   if (mHost->GetPrivateConfig(group, wxT("Chunk"), value, wxEmptyString))
   {
      char *buf = new char[value.length() / 4 * 3];

      int len = VSTEffect::b64decode(value, buf);
      if (len)
      {
         callDispatcher(effSetChunk, 1, len, buf, 0.0);
      }

      delete [] buf;

      return;
   }

   if (mHost->GetPrivateConfig(group, wxT("Value"), value, wxEmptyString))
   {
      wxStringTokenizer st(value, wxT(','));
      for (int i = 0; st.HasMoreTokens(); i++)
      {
         double val = 0.0;
         st.GetNextToken().ToDouble(&val);

         if (val >= -1.0 && val <= 1.0)
         {
            callSetParameter(i, val);
         }
      }
   }
}

void VSTEffect::SaveParameters(const wxString & group)
{
   if (mAEffect->flags & effFlagsProgramChunks)
   {
      void *chunk = NULL;
      int clen = (int) callDispatcher(effGetChunk, 1, 0, &chunk, 0.0);
      if (clen > 0)
      {
         mHost->SetPrivateConfig(group, wxT("Chunk"), VSTEffect::b64encode(chunk, clen));
         return;
      }
   }

   wxString parms;
   for (int i = 0; i < mAEffect->numParams; i++)
   {
      parms += wxString::Format(wxT(",%f"), callGetParameter(i));
   }

   mHost->SetPrivateConfig(group, wxT("Value"), parms.Mid(1));
}

void VSTEffect::OnTimer()
{
   wxRecursionGuard guard(mTimerGuard);

   // Ignore it if we're recursing
   if (guard.IsInside())
   {
      return;
   }

   if (mWantsIdle)
   {
      int ret = callDispatcher(effIdle, 0, 0, NULL, 0.0);
      if (!ret)
      {
         mWantsIdle = false;
      }
   }

   if (mWantsEditIdle)
   {
      callDispatcher(effEditIdle, 0, 0, NULL, 0.0);
   }
}

void VSTEffect::NeedIdle()
{
   mWantsIdle = true;
   mTimer->Start(100);
}

void VSTEffect::NeedEditIdle(bool state)
{
   mWantsEditIdle = state;
   mTimer->Start(100);
}

VstTimeInfo *VSTEffect::GetTimeInfo()
{
   mTimeInfo.nanoSeconds = wxGetLocalTimeMillis().ToDouble();
   return &mTimeInfo;
}

float VSTEffect::GetSampleRate()
{
   return mTimeInfo.sampleRate;
}

int VSTEffect::GetProcessLevel()
{
   return mProcessLevel;
}

void VSTEffect::PowerOn()
{
   if (!mHasPower)
   {
      // Turn the power on
      callDispatcher(effMainsChanged, 0, 1, NULL, 0.0);

      // Tell the effect we're going to start processing
      callDispatcher(effStartProcess, 0, 0, NULL, 0.0);

      // Set state
      mHasPower = true;
   }
}

void VSTEffect::PowerOff()
{
   if (mHasPower)
   {
      // Tell the effect we're going to stop processing
      callDispatcher(effStopProcess, 0, 0, NULL, 0.0);

      // Turn the power off
      callDispatcher(effMainsChanged, 0, 0, NULL, 0.0);

      // Set state
      mHasPower = false;
   }
}

void VSTEffect::SizeWindow(int w, int h)
{
   // Queue the event to make the resizes smoother
   if (mDlg)
   {
      wxCommandEvent sw(EVT_SIZEWINDOW);
      sw.SetInt(w);
      sw.SetExtraLong(h);
      mDlg->GetEventHandler()->AddPendingEvent(sw);
   }

   return;
}

void VSTEffect::UpdateDisplay()
{
   // Tell the dialog to refresh effect information
   if (mDlg)
   {
      wxCommandEvent ud(EVT_UPDATEDISPLAY);
      mDlg->GetEventHandler()->AddPendingEvent(ud);
   }

   return;
}

void VSTEffect::Automate(int index, float value)
{
   // Just ignore it if we're a slave
   if (mMaster)
   {
      return;
   }

   for (size_t i = 0, cnt = mSlaves.GetCount(); i < cnt; i++)
   {
      mSlaves[i]->callSetParameter(index, value);
   }

   return;
}

void VSTEffect::SetBufferDelay(int samples)
{
   // We do not support negative delay
   if (samples >= 0 && mUseBufferDelay)
   {
      mBufferDelay = samples;
   }

   return;
}

int VSTEffect::GetString(wxString & outstr, int opcode, int index)
{
   char buf[256];

   memset(buf, 0, sizeof(buf));

   callDispatcher(opcode, index, 0, buf, 0.0);

   outstr = wxString::FromUTF8(buf);

   return 0;
}

wxString VSTEffect::GetString(int opcode, int index)
{
   wxString str;

   GetString(str, opcode, index);

   return str;
}

void VSTEffect::SetString(int opcode, const wxString & str, int index)
{
   char buf[256];
   strcpy(buf, str.Left(255).ToUTF8());

   callDispatcher(opcode, index, 0, buf, 0.0);
}

intptr_t VSTEffect::callDispatcher(int opcode,
                                   int index, intptr_t value, void *ptr, float opt)
{
   // Needed since we might be in the dispatcher when the timer pops
   wxCRIT_SECT_LOCKER(locker, mDispatcherLock);
   return mAEffect->dispatcher(mAEffect, opcode, index, value, ptr, opt);
}

void VSTEffect::callProcessReplacing(float **inputs,
                                     float **outputs, int sampleframes)
{
   mAEffect->processReplacing(mAEffect, inputs, outputs, sampleframes);
}

float VSTEffect::callGetParameter(int index)
{
   return mAEffect->getParameter(mAEffect, index);
}

void VSTEffect::callSetParameter(int index, float value)
{
   mAEffect->setParameter(mAEffect, index, value);

   for (size_t i = 0, cnt = mSlaves.GetCount(); i < cnt; i++)
   {
      mSlaves[i]->callSetParameter(index, value);
   }
}

void VSTEffect::callSetProgram(int index)
{
   callDispatcher(effSetProgram, 0, index, NULL, 0.0);

   for (size_t i = 0, cnt = mSlaves.GetCount(); i < cnt; i++)
   {
      mSlaves[i]->callSetProgram(index);
   }
}

////////////////////////////////////////////////////////////////////////////////
// Base64 en/decoding
//
// Original routines marked as public domain and found at:
//
// http://en.wikibooks.org/wiki/Algorithm_implementation/Miscellaneous/Base64
//
////////////////////////////////////////////////////////////////////////////////

// Lookup table for encoding
const static wxChar cset[] = wxT("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/");
const static char padc = wxT('=');

wxString VSTEffect::b64encode(const void *in, int len)
{
   unsigned char *p = (unsigned char *) in;
   wxString out;

   unsigned long temp;
   for (int i = 0; i < len / 3; i++)
   {
      temp  = (*p++) << 16; //Convert to big endian
      temp += (*p++) << 8;
      temp += (*p++);
      out += cset[(temp & 0x00FC0000) >> 18];
      out += cset[(temp & 0x0003F000) >> 12];
      out += cset[(temp & 0x00000FC0) >> 6];
      out += cset[(temp & 0x0000003F)];
   }

   switch (len % 3)
   {
      case 1:
         temp  = (*p++) << 16; //Convert to big endian
         out += cset[(temp & 0x00FC0000) >> 18];
         out += cset[(temp & 0x0003F000) >> 12];
         out += padc;
         out += padc;
      break;

      case 2:
         temp  = (*p++) << 16; //Convert to big endian
         temp += (*p++) << 8;
         out += cset[(temp & 0x00FC0000) >> 18];
         out += cset[(temp & 0x0003F000) >> 12];
         out += cset[(temp & 0x00000FC0) >> 6];
         out += padc;
      break;
   }

   return out;
}

int VSTEffect::b64decode(wxString in, void *out)
{
   int len = in.length();
   unsigned char *p = (unsigned char *) out;

   if (len % 4)  //Sanity check
   {
      return 0;
   }

   int padding = 0;
   if (len)
   {
      if (in[len - 1] == padc)
      {
         padding++;
      }

      if (in[len - 2] == padc)
      {
         padding++;
      }
   }

   //const char *a = in.mb_str();
   //Setup a vector to hold the result
   unsigned long temp = 0; //Holds decoded quanta
   int i = 0;
   while (i < len)
   {
      for (int quantumPosition = 0; quantumPosition < 4; quantumPosition++)
      {
         unsigned char c = in[i];
         temp <<= 6;

         if (c >= 0x41 && c <= 0x5A)
         {
            temp |= c - 0x41;
         }
         else if (c >= 0x61 && c <= 0x7A)
         {
            temp |= c - 0x47;
         }
         else if (c >= 0x30 && c <= 0x39)
         {
            temp |= c + 0x04;
         }
         else if (c == 0x2B)
         {
            temp |= 0x3E;
         }
         else if (c == 0x2F)
         {
            temp |= 0x3F;
         }
         else if (c == padc)
         {
            switch (len - i)
            {
               case 1: //One pad character
                  *p++ = (temp >> 16) & 0x000000FF;
                  *p++ = (temp >> 8) & 0x000000FF;
                  return p - (unsigned char *) out;
               case 2: //Two pad characters
                  *p++ = (temp >> 10) & 0x000000FF;
                  return p - (unsigned char *) out;
            }
         }
         i++;
      }
      *p++ = (temp >> 16) & 0x000000FF;
      *p++ = (temp >> 8) & 0x000000FF;
      *p++ = temp & 0x000000FF;
   }

   return p - (unsigned char *) out;
}

#endif // USE_VST
