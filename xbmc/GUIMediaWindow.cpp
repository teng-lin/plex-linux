/*
 *      Copyright (C) 2005-2008 Team XBMC
 *      http://www.xbmc.org
 *
 *  This Program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  This Program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with XBMC; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *  http://www.gnu.org/copyleft/gpl.html
 *
 */

#include "GUIMediaWindow.h"
#include "HTTP.h"
#include "GUIUserMessages.h"
#include "Util.h"
#include "PlayListPlayer.h"
#include "addons/AddonManager.h"
#include "addons/PluginSource.h"
#include "FileSystem/ZipManager.h"
#include "FileSystem/PluginDirectory.h"
#include "FileSystem/MultiPathDirectory.h"
#include "GUIPassword.h"
#include "Application.h"
#include "utils/Network.h"
#include "utils/RegExp.h"
#include "PartyModeManager.h"
#include "GUIDialogMediaSource.h"
#include "GUIWindowFileManager.h"
#include "GUIDialogYesNo.h"
#include "Favourites.h"
#include "DirectoryCache.h"
#include "utils/LabelFormatter.h"
#include "GUIDialogProgress.h"
#include "AdvancedSettings.h"
#include "GUISettings.h"

#include "GUIDialogSmartPlaylistEditor.h"
#include "GUIDialogAddonSettings.h"
#include "GUIDialogYesNo.h"
#include "GUIWindowManager.h"
#include "GUIDialogOK.h"
#include "GUIDialogRating.h"
#include "PlayList.h"
#include "MediaManager.h"
#include "Settings.h"
#include "StringUtils.h"
#include "LocalizeStrings.h"
#include "utils/TimeUtils.h"
#include "FactoryFileDirectory.h"
#include "utils/log.h"
#include "utils/FileUtils.h"
#include "GUIEditControl.h"
#include "GUIDialogKeyboard.h"
#include "GUIDialogPlexPluginSettings.h"
#include "GUIDialogContextMenu.h"
#ifdef HAS_PYTHON
#include "lib/libPython/XBPython.h"
#endif
#include "utils/Builtins.h"
#include "PlexMediaServerQueue.h"
#include "PlexSourceScanner.h"

#define CONTROL_BTNVIEWASICONS     2
#define CONTROL_BTNSORTBY          3
#define CONTROL_BTNSORTASC         4
#define CONTROL_BTN_FILTER        19

#define CONTROL_LABELFILES        12

#define DEFAULT_MODE_FOR_DISABLED_VIEWS 65586

using namespace std;
using namespace ADDON;

class MediaRefresher : public CThread
{
public:
  
  MediaRefresher(const string& path)
  : m_path(path)
  , m_doneLoading(false)
  , m_canDie(false)
  {
    Create(true);
  }
  
  virtual void Process()
  {
    // Execute the request.
    CPlexDirectory plexDir(true, false);
    plexDir.GetDirectory(m_path, m_itemList);
    m_doneLoading = true;
    
    // Wait until I can die.
    while (m_canDie == false)
      Sleep(100);
  }
  
  bool            isDone() const { return m_doneLoading; }
  CFileItemList&  getItemList()  { return m_itemList;    }
  void            die()          { m_canDie = true;      }
  
private:
  
  string        m_path;
  CFileItemList m_itemList;
  volatile bool m_doneLoading;
  volatile bool m_canDie;
};

CGUIMediaWindow::CGUIMediaWindow(int id, const char *xmlFile)
    : CGUIWindow(id, xmlFile)
{
  m_vecItems = new CFileItemList;
  m_unfilteredItems = new CFileItemList;
  m_vecItems->m_strPath = "?";
  m_iLastControl = -1;
  m_iSelectedItem = -1;
  m_mediaRefresher = NULL;

  m_guiState.reset(CGUIViewState::GetViewState(GetID(), *m_vecItems));
}

CGUIMediaWindow::~CGUIMediaWindow()
{
  if (m_mediaRefresher)
    m_mediaRefresher->die();
  
  delete m_vecItems;
  delete m_unfilteredItems;
}

#define CONTROL_VIEW_START        50
#define CONTROL_VIEW_END          59

void CGUIMediaWindow::LoadAdditionalTags(TiXmlElement *root)
{
  CGUIWindow::LoadAdditionalTags(root);
  // configure our view control
  m_viewControl.Reset();
  m_viewControl.SetParentWindow(GetID());
  TiXmlElement *element = root->FirstChildElement("views");
  if (element && element->FirstChild())
  { // format is <views>50,29,51,95</views>
    CStdString allViews = element->FirstChild()->Value();
    CStdStringArray views;
    StringUtils::SplitString(allViews, ",", views);
    for (unsigned int i = 0; i < views.size(); i++)
    {
      int controlID = atol(views[i].c_str());
      CGUIControl *control = (CGUIControl *)GetControl(controlID);
      if (control && control->IsContainer())
        m_viewControl.AddView(control);
    }
  }
  else
  { // backward compatibility
    vector<CGUIControl *> controls;
    GetContainers(controls);
    for (ciControls it = controls.begin(); it != controls.end(); it++)
    {
      CGUIControl *control = *it;
      if (control->GetID() >= CONTROL_VIEW_START && control->GetID() <= CONTROL_VIEW_END)
        m_viewControl.AddView(control);
    }
  }
  m_viewControl.SetViewControlID(CONTROL_BTNVIEWASICONS);
}

void CGUIMediaWindow::OnWindowLoaded()
{
  SendMessage(GUI_MSG_SET_TYPE, CONTROL_BTN_FILTER, CGUIEditControl::INPUT_TYPE_FILTER);
  CGUIWindow::OnWindowLoaded();
  SetupShares();
}

void CGUIMediaWindow::OnWindowUnload()
{
  CGUIWindow::OnWindowUnload();
  m_viewControl.Reset();
}

CFileItemPtr CGUIMediaWindow::GetCurrentListItem(int offset)
{
  int item = m_viewControl.GetSelectedItem();
  if (!m_vecItems->Size() || item < 0)
    return CFileItemPtr();
  item = (item + offset) % m_vecItems->Size();
  if (item < 0) item += m_vecItems->Size();
  return m_vecItems->Get(item);
}

bool CGUIMediaWindow::OnAction(const CAction &action)
{
  if (action.GetID() == ACTION_PARENT_DIR)
  {
    if ((m_vecItems->IsVirtualDirectoryRoot() || m_vecItems->m_strPath == m_startDirectory) && g_advancedSettings.m_bUseEvilB)
    {
      m_vecItems->m_strPath="";
      m_history.ClearPathHistory();
      g_windowManager.PreviousWindow();
    }
    else
      GoParentFolder();
    return true;
  }

  if (action.GetID() == ACTION_PREVIOUS_MENU)
  {
    g_windowManager.PreviousWindow();
    return true;
  }

  // the non-contextual menu can be called at any time
  if (action.GetID() == ACTION_CONTEXT_MENU && !m_viewControl.HasControl(GetFocusedControlID()))
  {
    OnPopupMenu(-1);
    return true;
  }

  if (CGUIWindow::OnAction(action))
    return true;

  // live filtering
  if (action.GetID() == ACTION_FILTER_CLEAR)
  {
    CGUIMessage message(GUI_MSG_NOTIFY_ALL, GetID(), 0, GUI_MSG_FILTER_ITEMS);
    message.SetStringParam("");
    OnMessage(message);
    return true;
  }

  if (action.GetID() == ACTION_BACKSPACE)
  {
    CGUIMessage message(GUI_MSG_NOTIFY_ALL, GetID(), 0, GUI_MSG_FILTER_ITEMS, 2); // 2 for delete
    OnMessage(message);
    return true;
  }

  if (action.GetID() >= ACTION_FILTER_SMS2 && action.GetID() <= ACTION_FILTER_SMS9)
  {
    CStdString filter;
    filter.Format("%i", (int)(action.GetID() - ACTION_FILTER_SMS2 + 2));
    CGUIMessage message(GUI_MSG_NOTIFY_ALL, GetID(), 0, GUI_MSG_FILTER_ITEMS, 1); // 1 for append
    message.SetStringParam(filter);
    OnMessage(message);
    return true;
  }

  return false;
}

void CGUIMediaWindow::RefreshShares(bool update)
{
  if (m_vecItems->IsVirtualDirectoryRoot() && IsActive())
  {
    CPlexSourceScanner::MergeSourcesForWindow(GetID());
    SetupShares();
    
    if (update)
    {
      int iItem = m_viewControl.GetSelectedItem();
      Update(m_vecItems->m_strPath);
      m_viewControl.SetSelectedItem(iItem);
    }
  }
}

bool CGUIMediaWindow::OnMessage(CGUIMessage& message)
{
  switch ( message.GetMessage() )
  {
  case GUI_MSG_WINDOW_DEINIT:
    {
      m_iSelectedItem = m_viewControl.GetSelectedItem();
      m_iLastControl = GetFocusedControlID();
      
      if (m_refreshTimer.IsRunning())
        m_refreshTimer.Stop();
      
      if (m_mediaRefresher)
      {
        m_mediaRefresher->die();
        m_mediaRefresher = NULL;
      }
      
      CGUIWindow::OnMessage(message);
      // Call ClearFileItems() after our window has finished doing any WindowClose
      // animations
      ClearFileItems();
      return true;
    }
    break;

  case GUI_MSG_CLICKED:
    {
      int iControl = message.GetSenderId();
      if (iControl == CONTROL_BTNVIEWASICONS)
      {
        // view as control could be a select button
        int viewMode = 0;
        const CGUIControl *control = GetControl(CONTROL_BTNVIEWASICONS);
        if (control && control->GetControlType() != CGUIControl::GUICONTROL_BUTTON)
        {
          CGUIMessage msg(GUI_MSG_ITEM_SELECTED, GetID(), CONTROL_BTNVIEWASICONS);
          OnMessage(msg);
          viewMode = m_viewControl.GetViewModeNumber(msg.GetParam1());
        }
        else
          viewMode = m_viewControl.GetNextViewMode();

        if (m_guiState.get())
          m_guiState->SaveViewAsControl(viewMode);
        UpdateButtons();
        return true;
      }
      else if (iControl == CONTROL_BTNSORTASC) // sort asc
      {
        if (m_guiState.get())
          m_guiState->SetNextSortOrder();
        UpdateFileList();
        return true;
      }
      else if (iControl == CONTROL_BTNSORTBY) // sort by
      {
        if (m_guiState.get())
          m_guiState->SetNextSortMethod();
        UpdateFileList();
        return true;
      }
      else if (iControl == CONTROL_BTN_FILTER)
      {
        if (GetControl(iControl)->GetControlType() == CGUIControl::GUICONTROL_EDIT)
        { // filter updated
          CGUIMessage selected(GUI_MSG_ITEM_SELECTED, GetID(), CONTROL_BTN_FILTER);
          OnMessage(selected);
          OnFilterItems(selected.GetLabel());
          return true;
        }
        if (GetProperty("filter").IsEmpty())
        {
          CStdString filter = GetProperty("filter");
          CGUIDialogKeyboard::ShowAndGetFilter(filter, false);
          SetProperty("filter", filter);
        }
        else
          OnFilterItems("");
        return true;
      }
      else if (iControl == CONTENT_LIST_FILTERS)
      {
        // See what filter was selected and execute it.
        CGUIBaseContainer* control = (CGUIBaseContainer* )GetControl(CONTENT_LIST_FILTERS);
        
        int selected = control->GetSelectedItem();
        CFileItemPtr filterItem = CPlexDirectory::GetFilterList()->Get(selected);        
        Update(filterItem->m_strPath);
      }
      else if (m_viewControl.HasControl(iControl))  // list/thumb control
      {
        int iItem = m_viewControl.GetSelectedItem();
        int iAction = message.GetParam1();
        if (iItem < 0) break;
        if (iAction == ACTION_SELECT_ITEM || iAction == ACTION_MOUSE_LEFT_CLICK)
        {
          OnSelect(iItem);
        }
        else if (iAction == ACTION_CONTEXT_MENU || iAction == ACTION_MOUSE_RIGHT_CLICK)
        {
          OnPopupMenu(iItem);
          return true;
        }
      }
    }
    break;

  case GUI_MSG_SETFOCUS:
    {
      if (m_viewControl.HasControl(message.GetControlId()) && m_viewControl.GetCurrentControl() != message.GetControlId())
      {
        m_viewControl.SetFocused();
        return true;
      }
    }
    break;

  case GUI_MSG_NOTIFY_ALL:
    { // Message is received even if this window is inactive
      if (message.GetParam1() == GUI_MSG_WINDOW_RESET)
      {
        m_vecItems->m_strPath = "?";
        return true;
      }
      else if ( message.GetParam1() == GUI_MSG_REFRESH_THUMBS )
      {
        for (int i = 0; i < m_vecItems->Size(); i++)
          m_vecItems->Get(i)->FreeMemory(true);
        break;  // the window will take care of any info images
      }
      else if (message.GetParam1() == GUI_MSG_REMOVED_MEDIA)
      {
        if (m_vecItems->IsVirtualDirectoryRoot() && IsActive())
        {
          int iItem = m_viewControl.GetSelectedItem();
          Update(m_vecItems->m_strPath);
          m_viewControl.SetSelectedItem(iItem);
        }
        else if (m_vecItems->IsRemovable())
        { // check that we have this removable share still
          if (!m_rootDir.IsInSource(m_vecItems->m_strPath))
          { // don't have this share any more
            if (IsActive()) Update("");
            else
            {
              m_history.ClearPathHistory();
              m_vecItems->m_strPath="";
            }
          }
        }

        return true;
      }
      else if (message.GetParam1()==GUI_MSG_UPDATE_SOURCES)
      { 
        // State of the sources changed, so update our view
        RefreshShares(true);
        return true;
      }
      else if (message.GetParam1()==GUI_MSG_UPDATE_REMOTE_SOURCES)
      {
        RefreshShares(true);
        return true;
      }
      else if (message.GetParam1()==GUI_MSG_UPDATE && IsActive())
      {
        if (message.GetNumStringParams())
        {
          m_vecItems->m_strPath = message.GetStringParam();
          if (message.GetParam2()) // param2 is used for resetting the history
            SetHistoryForPath(m_vecItems->m_strPath);
        }
        // clear any cached listing
        m_vecItems->RemoveDiscCache(GetID());
        Update(m_vecItems->m_strPath);
      }
      else if (message.GetParam1()==GUI_MSG_UPDATE_ITEM && message.GetItem())
      {
        CFileItemPtr newItem = boost::static_pointer_cast<CFileItem>(message.GetItem());
        if (IsActive())
        {
          if (m_vecItems->UpdateItem(newItem.get()) && message.GetParam2() == 1)
          { // need the list updated as well
            UpdateFileList();
          }
        }
        else if (newItem)
        { // need to remove the disc cache
          CFileItemList items;
          CUtil::GetDirectory(newItem->m_strPath, items.m_strPath);
          items.RemoveDiscCache(GetID());
        }
      }
      else if (message.GetParam1()==GUI_MSG_UPDATE_PATH)
      {
        if (IsActive())
        {
          if((message.GetStringParam() == m_vecItems->m_strPath) ||
             (m_vecItems->IsMultiPath() && XFILE::CMultiPathDirectory::HasPath(m_vecItems->m_strPath, message.GetStringParam())))
          {
            Update(m_vecItems->m_strPath);
          }
        }
      }
      else if (message.GetParam1() == GUI_MSG_FILTER_ITEMS && IsActive())
      {
        CStdString filter(GetProperty("filter"));
        if (message.GetParam2() == 1) // append
          filter += message.GetStringParam();
        else if (message.GetParam2() == 2)
        { // delete
          if (filter.size())
            filter = filter.Left(filter.size() - 1);
        }
        else
          filter = message.GetStringParam();
        OnFilterItems(filter);
        return true;
      }
      else
        return CGUIWindow::OnMessage(message);

      return true;
    }
    break;
  case GUI_MSG_PLAYBACK_STARTED:
  case GUI_MSG_PLAYBACK_ENDED:
  case GUI_MSG_PLAYBACK_STOPPED:
  case GUI_MSG_PLAYLIST_CHANGED:
  case GUI_MSG_PLAYLISTPLAYER_STOPPED:
  case GUI_MSG_PLAYLISTPLAYER_STARTED:
  case GUI_MSG_PLAYLISTPLAYER_CHANGED:
    { // send a notify all to all controls on this window
      CGUIMessage msg(GUI_MSG_NOTIFY_ALL, GetID(), 0, GUI_MSG_REFRESH_LIST);
      OnMessage(msg);
      break;
    }
  case GUI_MSG_CHANGE_VIEW_MODE:
    {
      int viewMode = 0;
      if (message.GetParam1())  // we have an id
        viewMode = m_viewControl.GetViewModeByID(message.GetParam1());
      else if (message.GetParam2())
        viewMode = m_viewControl.GetNextViewMode((int)message.GetParam2());
      
      // Notify the media server.
      PlexMediaServerQueue::Get().onViewModeChanged(m_vecItems->GetProperty("identifier"), m_vecItems->m_strPath, m_vecItems->GetProperty("viewGroup"), viewMode, -1, -1);
      m_vecItems->SetDefaultViewMode(viewMode);

      UpdateButtons();
      return true;
    }
    break;
  case GUI_MSG_CHANGE_SORT_METHOD:
    {
      if (m_guiState.get())
      {
        if (message.GetParam1())
          m_guiState->SetCurrentSortMethod((int)message.GetParam1());
        else if (message.GetParam2())
          m_guiState->SetNextSortMethod((int)message.GetParam2());
      }
      UpdateFileList();
      return true;
    }
    break;
  case GUI_MSG_CHANGE_SORT_DIRECTION:
    {
      if (m_guiState.get())
        m_guiState->SetNextSortOrder();
      UpdateFileList();
      return true;
    }
    break;
  case GUI_MSG_WINDOW_INIT:
    {
      if (m_vecItems->m_strPath == "?")
        m_vecItems->m_strPath.Empty();
      CStdString dir = message.GetStringParam(0);
      const CStdString &ret = message.GetStringParam(1);
      bool returning = ret.CompareNoCase("return") == 0;
      if (!dir.IsEmpty())
      {
        m_history.ClearPathHistory();
        // ensure our directory is valid
        dir = GetStartFolder(dir);
        if (!returning || m_vecItems->m_strPath.Left(dir.GetLength()) != dir)
        { // we're not returning to the same path, so set our directory to the requested path
          m_vecItems->m_strPath = dir;
        }
        // check for network up
        if (CUtil::IsRemote(m_vecItems->m_strPath) && !WaitForNetwork())
          m_vecItems->m_strPath.Empty();
        SetHistoryForPath(m_vecItems->m_strPath);
      }
      if (message.GetParam1() != WINDOW_INVALID)
      { // first time to this window - make sure we set the root path
        m_startDirectory = returning ? dir : "";
      }
    }
    break;
  }

  return CGUIWindow::OnMessage(message);
}

// \brief Updates the states (enable, disable, visible...)
// of the controls defined by this window
// Override this function in a derived class to add new controls
void CGUIMediaWindow::UpdateButtons()
{
  if (m_guiState.get())
  {
    // Update sorting controls
    if (m_guiState->GetDisplaySortOrder()==SORT_ORDER_NONE)
    {
      CONTROL_DISABLE(CONTROL_BTNSORTASC);
    }
    else
    {
      CONTROL_ENABLE(CONTROL_BTNSORTASC);
      if (m_guiState->GetDisplaySortOrder()==SORT_ORDER_ASC)
      {
        CGUIMessage msg(GUI_MSG_DESELECTED, GetID(), CONTROL_BTNSORTASC);
        g_windowManager.SendMessage(msg);
      }
      else
      {
        CGUIMessage msg(GUI_MSG_SELECTED, GetID(), CONTROL_BTNSORTASC);
        g_windowManager.SendMessage(msg);
      }
    }

    // Update list/thumb control if the selected view isn't disabled
    bool allowChange = true;
    int viewMode = m_guiState->GetViewAsControl();
    
    // Check the list of disabled view modes for this directory
    CStdStringArray viewModes;
    StringUtils::SplitString(CurrentDirectory().GetDisabledViewModes(), ",", viewModes);
    for (unsigned int i = 0; i < viewModes.size(); i++)
    {
      if (atoi(viewModes[i]) == viewMode)
        allowChange = false;
    }
    
    // If we have a default view mode, use that instead
    if (CurrentDirectory().GetDefaultViewMode() > 0)
      m_viewControl.SetCurrentView(CurrentDirectory().GetDefaultViewMode());
    
    // Otherwise, use the global default
    else
      m_viewControl.SetCurrentView(DEFAULT_MODE_FOR_DISABLED_VIEWS);

    // Update sort by button
    if (m_guiState->GetSortMethod()==SORT_METHOD_NONE)
    {
      CONTROL_DISABLE(CONTROL_BTNSORTBY);
    }
    else
    {
      CONTROL_ENABLE(CONTROL_BTNSORTBY);
    }
    CStdString sortLabel;
    sortLabel.Format(g_localizeStrings.Get(550).c_str(), g_localizeStrings.Get(m_guiState->GetSortMethodLabel()).c_str());
    SET_CONTROL_LABEL(CONTROL_BTNSORTBY, sortLabel);
  }

  CStdString items;
  items.Format("%i %s", m_vecItems->GetObjectCount(), g_localizeStrings.Get(127).c_str());
  SET_CONTROL_LABEL(CONTROL_LABELFILES, items);

  //#ifdef PRE_SKIN_VERSION_3
  SET_CONTROL_SELECTED(GetID(),CONTROL_BTN_FILTER, !GetProperty("filter").IsEmpty());
  SET_CONTROL_LABEL2(CONTROL_BTN_FILTER, GetProperty("filter"));
  //#endif
}

void CGUIMediaWindow::ClearFileItems()
{
  m_viewControl.Clear();
  int defaultViewMode = m_vecItems->GetDefaultViewMode();
  m_vecItems->Clear(); // will clean up everything
  m_vecItems->SetDefaultViewMode(defaultViewMode);
  
  m_unfilteredItems->Clear();
}

// \brief Sorts Fileitems based on the sort method and sort oder provided by guiViewState
void CGUIMediaWindow::SortItems(CFileItemList &items)
{
#if 0
  auto_ptr<CGUIViewState> guiState(CGUIViewState::GetViewState(GetID(), items));
  if (guiState.get())
  {
    items.Sort(guiState->GetSortMethod(), guiState->GetDisplaySortOrder());

    // Should these items be saved to the hdd
    if (items.CacheToDiscAlways())
      items.Save(GetID());
  }
#endif
}

// \brief Formats item labels based on the formatting provided by guiViewState
void CGUIMediaWindow::FormatItemLabels(CFileItemList &items, const LABEL_MASKS &labelMasks)
{
  CLabelFormatter fileFormatter(labelMasks.m_strLabelFile, labelMasks.m_strLabel2File);
  CLabelFormatter folderFormatter(labelMasks.m_strLabelFolder, labelMasks.m_strLabel2Folder);
  for (int i=0; i<items.Size(); ++i)
  {
    CFileItemPtr pItem=items[i];

    if (pItem->IsLabelPreformated())
      continue;

    if (pItem->m_bIsFolder)
      folderFormatter.FormatLabels(pItem.get());
    else
      fileFormatter.FormatLabels(pItem.get());
  }

  if(items.GetSortMethod() == SORT_METHOD_LABEL_IGNORE_THE
  || items.GetSortMethod() == SORT_METHOD_LABEL)
    items.ClearSortState();
}

// \brief Prepares and adds the fileitems list/thumb panel
void CGUIMediaWindow::FormatAndSort(CFileItemList &items)
{
  auto_ptr<CGUIViewState> viewState(CGUIViewState::GetViewState(GetID(), items));

  if (viewState.get())
  {
    LABEL_MASKS labelMasks;
    viewState->GetSortMethodLabelMasks(labelMasks);
    FormatItemLabels(items, labelMasks);
  }
  SortItems(items);
}

/*!
  \brief Overwrite to fill fileitems from a source
  \param strDirectory Path to read
  \param items Fill with items specified in \e strDirectory
  */
bool CGUIMediaWindow::GetDirectory(const CStdString &strDirectory, CFileItemList &items)
{
  // cleanup items
  if (items.Size())
    items.Clear();

  CStdString strParentPath=m_history.GetParentPath();

  CLog::Log(LOGDEBUG,"CGUIMediaWindow::GetDirectory (%s)", strDirectory.c_str());
  CLog::Log(LOGDEBUG,"  ParentPath = [%s]", strParentPath.c_str());

  // see if we can load a previously cached folder
  CFileItemList cachedItems(strDirectory);
  if (!strDirectory.IsEmpty() && cachedItems.Load(GetID()))
  {
    items.Assign(cachedItems);
  }
  else
  {
    unsigned int time = CTimeUtils::GetTimeMS();

    if (strDirectory.IsEmpty())
      SetupShares();

    if (!m_rootDir.GetDirectory(strDirectory, items))
      return false;

    // took over a second, and not normally cached, so cache it
    if (time + 1000 < CTimeUtils::GetTimeMS() && items.CacheToDiscIfSlow())
      items.Save(GetID());

    // if these items should replace the current listing, then pop it off the top
    if (items.GetReplaceListing())
      m_history.RemoveParentPath();
  }
  
  // PLEX: Default to plug-in stream for top-level.
  if (strDirectory.size() == 0)
  {
    int viewMode = 131131;
    items.SetDefaultViewMode(viewMode);
  }

  if (m_guiState.get() && !m_guiState->HideParentDirItems() && !items.m_strPath.IsEmpty())
  {
    CFileItemPtr pItem(new CFileItem(".."));
    pItem->m_strPath = strParentPath;
    pItem->m_bIsFolder = true;
    pItem->m_bIsShareOrDrive = false;
    items.AddFront(pItem, 0);
  }

  int iWindow = GetID();
  CStdStringArray regexps;

  if (iWindow == WINDOW_VIDEO_FILES)
    regexps = g_advancedSettings.m_videoExcludeFromListingRegExps;
  if (iWindow == WINDOW_MUSIC_FILES)
    regexps = g_advancedSettings.m_audioExcludeFromListingRegExps;
  if (iWindow == WINDOW_PICTURES)
    regexps = g_advancedSettings.m_pictureExcludeFromListingRegExps;

  if (regexps.size())
  {
    for (int i=0; i < items.Size();)
    {
      if (CUtil::ExcludeFileOrFolder(items[i]->m_strPath, regexps))
        items.Remove(i);
      else
        i++;
    }
  }

  // clear the filter
  SetProperty("filter", "");
  return true;
}

// \brief Set window to a specific directory
// \param strDirectory The directory to be displayed in list/thumb control
// This function calls OnPrepareFileItems() and OnFinalizeFileItems()
bool CGUIMediaWindow::Update(const CStdString &strDirectory)
{
  // TODO: OnInitWindow calls Update() before window path has been set properly.
  if (strDirectory == "?")
    return false;
  
  RefreshShares();

  // get selected item
  int iItem = m_viewControl.GetSelectedItem();
  CStdString strSelectedItem = "";
  if (iItem >= 0 && iItem < m_vecItems->Size())
  {
    CFileItemPtr pItem = m_vecItems->Get(iItem);
    if (!pItem->IsParentFolder())
    {
      GetDirectoryHistoryString(pItem.get(), strSelectedItem);
    }
  }

  CStdString strOldDirectory = m_vecItems->m_strPath;

  m_history.SetSelectedItem(strSelectedItem, strOldDirectory);

  CFileItemList items;
  
  // Save the default view mode.
  if (strOldDirectory == strDirectory)
    items.SetDefaultViewMode(m_vecItems->GetDefaultViewMode());
  
  if (!GetDirectory(strDirectory, items) || (items.m_displayMessage && items.Size() == 0))
  {
    if (items.m_displayMessage)
      CGUIDialogOK::ShowAndGetInput(items.m_displayMessageTitle, items.m_displayMessageContents, "", "");
    
    if (items.m_wasListingCancelled == true)
    {
      // Fast path.
      if (strDirectory.Equals(strOldDirectory) == false)
        m_history.RemoveParentPath();
      
      return true;
    }

    ClearFileItems();
    m_vecItems->ClearProperties();
    m_vecItems->SetThumbnailImage("");
    
    if (items.m_wasListingCancelled == false)
      CLog::Log(LOGERROR,"CGUIMediaWindow::GetDirectory(%s) failed", strDirectory.c_str());
    else
      CLog::Log(LOGINFO,"CGUIMediaWindow::GetDirectory(%s) was canceled", strDirectory.c_str());
    
    // if the directory is the same as the old directory, then we'll return
    // false.  Else, we assume we can get the previous directory
    if (strDirectory.Equals(strOldDirectory))
      return false;

    // We assume, we can get the parent
    // directory again, but we have to
    // return false to be able to eg. show
    // an error message.
    CStdString strParentPath = m_history.GetParentPath();
    m_history.RemoveParentPath();
    Update(strParentPath);
    
    
    if (strParentPath.IsEmpty())
      return true;
    
    // Note whether the listing was canceled or not.
    m_vecItems->m_wasListingCancelled = items.m_wasListingCancelled;
    
    return false;
  }

  ClearFileItems();
  m_vecItems->Copy(items);

  // Double check and see if we need to update.
  if (m_updatedItem &&
      m_vecItems->Get(m_iSelectedItem) &&
      m_updatedItem->GetProperty("ratingKey").size() > 0 &&
      m_updatedItem->GetProperty("ratingKey") == m_vecItems->Get(m_iSelectedItem)->GetProperty("ratingKey"))
  {
    // Update resume time and view count.
    CFileItemPtr item = m_vecItems->Get(m_iSelectedItem);
    if (m_updatedItem->HasProperty("viewOffset"))
      item->SetProperty("viewOffset", m_updatedItem->GetProperty("viewOffset"));
    else
      item->ClearProperty("viewOffset");
    
    // Play count.
    item->GetVideoInfoTag()->m_playCount = m_updatedItem->GetVideoInfoTag()->m_playCount;
    
    // Icon.
    item->SetOverlayImage((CGUIListItem::GUIIconOverlay)m_updatedItem->GetOverlayImageID());
    
    m_updatedItem = CFileItemPtr();
  }
  
  // if we're getting the root source listing
  // make sure the path history is clean
  if (strDirectory.IsEmpty())
    m_history.ClearPathHistory();

  int iWindow = GetID();
  bool bOkay = (iWindow == WINDOW_MUSIC_FILES || iWindow == WINDOW_VIDEO_FILES || iWindow == WINDOW_FILES || iWindow == WINDOW_PICTURES || iWindow == WINDOW_PROGRAMS);
  if (strDirectory.IsEmpty() && bOkay && (m_vecItems->Size() == 0 || !m_guiState->DisableAddSourceButtons())) // add 'add source button'
  {
#if 0
    CStdString strLabel = g_localizeStrings.Get(1026);
    CFileItemPtr pItem(new CFileItem(strLabel));
    pItem->m_strPath = "add";
    pItem->SetIconImage("DefaultAddSource.png");
    pItem->SetLabel(strLabel);
    pItem->SetLabelPreformated(true);
    m_vecItems->Add(pItem);
#endif
  }
  m_iLastControl = GetFocusedControlID();

  //  Ask the derived class if it wants to load additional info
  //  for the fileitems like media info or additional
  //  filtering on the items, setting thumbs.
  OnPrepareFileItems(*m_vecItems);

  m_vecItems->FillInDefaultIcons();

  m_guiState.reset(CGUIViewState::GetViewState(GetID(), *m_vecItems));

  FormatAndSort(*m_vecItems);

  // Ask the devived class if it wants to do custom list operations,
  // eg. changing the label
  OnFinalizeFileItems(*m_vecItems);
  UpdateButtons();

  m_viewControl.SetItems(*m_vecItems);

  strSelectedItem = m_history.GetSelectedItem(m_vecItems->m_strPath);

  bool bSelectedFound = false;
  //int iSongInDirectory = -1;
  for (int i = 0; i < m_vecItems->Size(); ++i)
  {
    CFileItemPtr pItem = m_vecItems->Get(i);

    // Update selected item
    if (!bSelectedFound)
    {
      CStdString strHistory;
      GetDirectoryHistoryString(pItem.get(), strHistory);
      if (strHistory == strSelectedItem)
      {
        m_viewControl.SetSelectedItem(i);
        bSelectedFound = true;
      }
    }
  }

  // if we haven't found the selected item, select the first item
  if (!bSelectedFound)
    m_viewControl.SetSelectedItem(0);

  m_history.AddPath(m_vecItems->m_strPath);

  //m_history.DumpPathHistory();
  
  // Make sure root directories end up with a content type of "plugins".
  if (m_vecItems->IsVirtualDirectoryRoot())
    m_vecItems->SetContent("plugins");

  // Last, but not least, make sure the filter list is bound.
  CGUIBaseContainer* control = (CGUIBaseContainer* )GetControl(CONTENT_LIST_FILTERS);
  if (control && CPlexDirectory::GetFilterList()->Size() > 0)
  {
    // Bind the list.
    CGUIMessage msg(GUI_MSG_LABEL_BIND, GetID(), CONTENT_LIST_FILTERS, 0, 0, CPlexDirectory::GetFilterList().get());
    OnMessage(msg);
  }
  
  return true;
}

// \brief This function will be called by Update() before the
// labels of the fileitems are formatted. Override this function
// to set custom thumbs or load additional media info.
// It's used to load tag info for music.
void CGUIMediaWindow::OnPrepareFileItems(CFileItemList &items)
{

}

// \brief This function will be called by Update() after the
// labels of the fileitems are formatted. Override this function
// to modify the fileitems. Eg. to modify the item label
void CGUIMediaWindow::OnFinalizeFileItems(CFileItemList &items)
{
  m_unfilteredItems->Append(items);
  
  CStdString filter(GetProperty("filter"));
  if (!filter.IsEmpty())
  {
    items.ClearItems();
    GetFilteredItems(filter, items);
  }

  // The idea here is to ensure we have something to focus if our file list
  // is empty.  As such, this check MUST be last and ignore the hide parent
  // fileitems settings.
  if (items.IsEmpty())
  {
    CFileItemPtr pItem(new CFileItem(".."));
    pItem->m_strPath=m_history.GetParentPath();
    pItem->m_bIsFolder = true;
    pItem->m_bIsShareOrDrive = false;
    items.AddFront(pItem, 0);
  }
  
  // Check whether the refresh timer is required
  if (m_vecItems->m_autoRefresh > 0)
  {
    if (!m_refreshTimer.IsRunning())
      m_refreshTimer.StartZero();
  }
  else
  {
    if (m_refreshTimer.IsRunning())
      m_refreshTimer.Stop();
  }
}

// \brief With this function you can react on a users click in the list/thumb panel.
// It returns true, if the click is handled.
// This function calls OnPlayMedia()
bool CGUIMediaWindow::OnClick(int iItem)
{
  if ( iItem < 0 || iItem >= (int)m_vecItems->Size() ) return true;
  CFileItemPtr pItem = m_vecItems->Get(iItem);

  if (pItem->IsParentFolder())
  {
    GoParentFolder();
    return true;
  }
  if (pItem->m_strPath == "add" && pItem->GetLabel() == g_localizeStrings.Get(1026)) // 'add source button' in empty root
  {
    OnContextButton(0, CONTEXT_BUTTON_ADD_SOURCE);
    return true;
  }

  if (pItem->m_bIsFolder)
  {
    if ( pItem->m_bIsShareOrDrive )
    {
      const CStdString& strLockType=m_guiState->GetLockType();
      if (g_settings.GetMasterProfile().getLockMode() != LOCK_MODE_EVERYONE)
        if (!strLockType.IsEmpty() && !g_passwordManager.IsItemUnlocked(pItem.get(), strLockType))
            return true;

      if (!HaveDiscOrConnection(pItem->m_strPath, pItem->m_iDriveType))
        return true;
    }

    // check for the partymode playlist items - they may not exist yet
    if ((pItem->m_strPath == g_settings.GetUserDataItem("PartyMode.xsp")) ||
        (pItem->m_strPath == g_settings.GetUserDataItem("PartyMode-Video.xsp")))
    {
      // party mode playlist item - if it doesn't exist, prompt for user to define it
      if (!XFILE::CFile::Exists(pItem->m_strPath))
      {
        m_vecItems->RemoveDiscCache(GetID());
        if (CGUIDialogSmartPlaylistEditor::EditPlaylist(pItem->m_strPath))
          Update(m_vecItems->m_strPath);
        return true;
      }
    }

    // remove the directory cache if the folder is not normally cached
    CFileItemList items(pItem->m_strPath);
    if (!items.AlwaysCache())
      items.RemoveDiscCache(GetID());

    CFileItem directory(*pItem);

    // Show on-screen keyboard for PMS search queries
    if (pItem->m_bIsSearchDir)
    {
      CStdString strSearchTerm = "";
      if (CGUIDialogKeyboard::ShowAndGetInput(strSearchTerm, pItem->m_strSearchPrompt, false))
      {
        // Encode the query.
        CUtil::URLEncode(strSearchTerm);
        
        // Find the ? if there is one.
        CStdString newURL = directory.m_strPath;
        CUtil::RemoveSlashAtEnd(newURL);
        
        newURL += (newURL.Find("?") > 0) ? "&" : "?";
        newURL += "query=" + strSearchTerm;
        directory.m_strPath = newURL;
      }
      else
      {
        // If no query was entered or the user dismissed the keyboard, do nothing
        return true;
      }
    }
    
    // Show a context menu for PMS popup directories
    if (pItem->m_bIsPopupMenuItem)
    {
      CFileItemList fileItems;
      CContextButtons buttons;
      CPlexDirectory plexDir;
      
      plexDir.GetDirectory(directory.m_strPath, fileItems);
      for ( int i = 0; i < fileItems.Size(); i++ )
      {
        CFileItemPtr item = fileItems.Get(i);
        buttons.Add(i, item->GetLabel());
      }
      
      int choice = CGUIDialogContextMenu::ShowAndGetChoice(buttons);
      if (choice >= 0)
      {
        CFileItemPtr selectedItem = fileItems.Get(choice);
        if (selectedItem->m_bIsFolder)
        {
          Update(selectedItem->m_strPath);
        }
        else
        { 
          selectedItem->SetLabel(pItem->GetLabel() + ": " + selectedItem->GetLabel());
          OnPlayMedia(selectedItem.get());
        }
      }
      return true;
    }
    
    // Show preferences.
    if (pItem->m_bIsSettingsDir)
    {
      CFileItemList fileItems;
      vector<CStdString> items;
      CPlexDirectory plexDir(false);
      
      plexDir.GetDirectory(directory.m_strPath, fileItems);
      CGUIDialogPlexPluginSettings::ShowAndGetInput(pItem->m_strPath, plexDir.GetData());
      
      Update(m_vecItems->m_strPath);
      return true;
    }

    if (!Update(directory.m_strPath))
      ShowShareErrorMessage(&directory);
    
    return true;
  }
  else if (pItem->IsPlugin() && pItem->GetProperty("isplayable") != "true")
  {
    return XFILE::CPluginDirectory::RunScriptWithParams(pItem->m_strPath);
  }
  else
  {
    m_iSelectedItem = m_viewControl.GetSelectedItem();

    if (pItem->m_strPath == "newplaylist://")
    {
      m_vecItems->RemoveDiscCache(GetID());
      g_windowManager.ActivateWindow(WINDOW_MUSIC_PLAYLIST_EDITOR,"newplaylist://");
      return true;
    }
    else if (pItem->m_strPath.Left(19).Equals("newsmartplaylist://"))
    {
      m_vecItems->RemoveDiscCache(GetID());
      if (CGUIDialogSmartPlaylistEditor::NewPlaylist(pItem->m_strPath.Mid(19)))
        Update(m_vecItems->m_strPath);
      return true;
    }
    else if (pItem->m_strPath.Left(14).Equals("addons://more/"))
    {
      CBuiltins::Execute("ActivateWindow(AddonBrowser,addons://all/xbmc.addon." + pItem->m_strPath.Mid(14) + ",return)");
      return true;
    }

    // If karaoke song is being played AND popup autoselector is enabled, the playlist should not be added
    bool do_not_add_karaoke = g_guiSettings.GetBool("karaoke.enabled") &&
      g_guiSettings.GetBool("karaoke.autopopupselector") && pItem->IsKaraoke();
    bool autoplay = m_guiState.get() && m_guiState->AutoPlayNextItem();
    int iPlaylist = m_guiState.get()?m_guiState->GetPlaylist():PLAYLIST_MUSIC;

    if (pItem->IsPlugin())
    {
      CURL url(pItem->m_strPath);
      AddonPtr addon;
      if (CAddonMgr::Get().GetAddon(url.GetHostName(),addon))
      {
        PluginPtr plugin = boost::dynamic_pointer_cast<CPluginSource>(addon);
        if (plugin && plugin->Provides(CPluginSource::AUDIO))
        {
          iPlaylist = PLAYLIST_MUSIC;
          autoplay = g_guiSettings.GetBool("musicplayer.autoplaynextitem");
        }
      }
    }

    if (autoplay && !g_partyModeManager.IsEnabled() && 
        !pItem->IsPlayList() && !do_not_add_karaoke)
    {
      g_playlistPlayer.ClearPlaylist(iPlaylist);
      g_playlistPlayer.Reset();
      int songToPlay = 0;
      CFileItemList queueItems;
      for ( int i = 0; i < m_vecItems->Size(); i++ )
      {
        CFileItemPtr item = m_vecItems->Get(i);

        if (item->m_bIsFolder)
          continue;

        if (!item->IsPlayList() && !item->IsZIP() && !item->IsRAR())
          queueItems.Add(item);

        if (item == pItem)
        { // item that was clicked
          songToPlay = queueItems.Size() - 1;
        }
      }
      g_playlistPlayer.Add(iPlaylist, queueItems);

      // Save current window and directory to know where the selected item was
      if (m_guiState.get())
        m_guiState->SetPlaylistDirectory(m_vecItems->m_strPath);

      // figure out where we start playback
      if (g_playlistPlayer.IsShuffled(iPlaylist))
      {
        int iIndex = g_playlistPlayer.GetPlaylist(iPlaylist).FindOrder(songToPlay);
        g_playlistPlayer.GetPlaylist(iPlaylist).Swap(0, iIndex);
        songToPlay = 0;
      }

      // play
      g_playlistPlayer.SetCurrentPlaylist(iPlaylist);
      g_playlistPlayer.Play(songToPlay);
      return true;
    }
    else
    {
      return OnPlayMedia(iItem);
    }
  }

  return false;
}

bool CGUIMediaWindow::OnSelect(int item)
{
  return OnClick(item);
}

// \brief Checks if there is a disc in the dvd drive and whether the
// network is connected or not.
bool CGUIMediaWindow::HaveDiscOrConnection(CStdString& strPath, int iDriveType)
{
  if (iDriveType==CMediaSource::SOURCE_TYPE_DVD)
  {
    if (!g_mediaManager.IsDiscInDrive())
    {
      CGUIDialogOK::ShowAndGetInput(218, 219, 0, 0);
      return false;
    }
  }
  else if (iDriveType==CMediaSource::SOURCE_TYPE_REMOTE)
  {
    // TODO: Handle not connected to a remote share
    if ( !g_application.getNetwork().IsConnected() )
    {
      CGUIDialogOK::ShowAndGetInput(220, 221, 0, 0);
      return false;
    }
  }

  return true;
}

// \brief Shows a standard errormessage for a given pItem.
void CGUIMediaWindow::ShowShareErrorMessage(CFileItem* pItem)
{
  if (pItem->m_bIsShareOrDrive)
  {
    int idMessageText=0;
    const CURL& url=pItem->GetAsUrl();
    const CStdString& strHostName=url.GetHostName();

    if (pItem->m_iDriveType != CMediaSource::SOURCE_TYPE_REMOTE) //  Local shares incl. dvd drive
      idMessageText=15300;
    else if (url.GetProtocol() == "xbms" && strHostName.IsEmpty()) //  xbms server discover
      idMessageText=15302;
    else if (url.GetProtocol() == "smb" && strHostName.IsEmpty()) //  smb workgroup
      idMessageText=15303;
    else  //  All other remote shares
      idMessageText=15301;

    CGUIDialogOK::ShowAndGetInput(220, idMessageText, 0, 0);
  }
}

// \brief The functon goes up one level in the directory tree
void CGUIMediaWindow::GoParentFolder()
{
  //m_history.DumpPathHistory();

  // remove current directory if its on the stack
  // there were some issues due some folders having a trailing slash and some not
  // so just add a trailing slash to all of them for comparison.
  CStdString strPath = m_vecItems->m_strPath;
  CUtil::AddSlashAtEnd(strPath);
  CStdString strParent = m_history.GetParentPath();
  // in case the path history is messed up and the current folder is on
  // the stack more than once, keep going until there's nothing left or they
  // dont match anymore.
  while (!strParent.IsEmpty())
  {
    CUtil::AddSlashAtEnd(strParent);
    if (strParent.Equals(strPath))
      m_history.RemoveParentPath();
    else
      break;
    strParent = m_history.GetParentPath();
  }

  // if vector is not empty, pop parent
  // if vector is empty, parent is root source listing
  CStdString strOldPath(m_vecItems->m_strPath);
  strParent = m_history.RemoveParentPath();
  Update(strParent);
}

// \brief Override the function to change the default behavior on how
// a selected item history should look like
void CGUIMediaWindow::GetDirectoryHistoryString(const CFileItem* pItem, CStdString& strHistoryString)
{
  if (pItem->m_bIsShareOrDrive)
  {
    // We are in the virual directory

    // History string of the DVD drive
    // must be handel separately
    if (pItem->m_iDriveType == CMediaSource::SOURCE_TYPE_DVD)
    {
      // Remove disc label from item label
      // and use as history string, m_strPath
      // can change for new discs
      CStdString strLabel = pItem->GetLabel();
      int nPosOpen = strLabel.Find('(');
      int nPosClose = strLabel.ReverseFind(')');
      if (nPosOpen > -1 && nPosClose > -1 && nPosClose > nPosOpen)
      {
        strLabel.Delete(nPosOpen + 1, (nPosClose) - (nPosOpen + 1));
        strHistoryString = strLabel;
      }
      else
        strHistoryString = strLabel;
    }
    else
    {
      // Other items in virual directory
      CStdString strPath = pItem->m_strPath;
      CUtil::RemoveSlashAtEnd(strPath);

      strHistoryString = pItem->GetLabel() + strPath;
    }
  }
  else if (pItem->m_lEndOffset>pItem->m_lStartOffset && pItem->m_lStartOffset != -1)
  {
    // Could be a cue item, all items of a cue share the same filename
    // so add the offsets to build the history string
    strHistoryString.Format("%ld%ld", pItem->m_lStartOffset, pItem->m_lEndOffset);
    strHistoryString += pItem->m_strPath;
  }
  else
  {
    // Normal directory items
    strHistoryString = pItem->m_strPath;
  }
  CUtil::RemoveSlashAtEnd(strHistoryString);
  strHistoryString.ToLower();
}

// \brief Call this function to create a directory history for the
// path given by strDirectory.
void CGUIMediaWindow::SetHistoryForPath(const CStdString& strDirectory)
{
  // Make sure our shares are configured
  SetupShares();
  if (!strDirectory.IsEmpty())
  {
    // Build the directory history for default path
    CStdString strPath, strParentPath;
    strPath = strDirectory;
    CUtil::RemoveSlashAtEnd(strPath);

    CFileItemList items;
    m_rootDir.GetDirectory("", items);

    m_history.ClearPathHistory();

    while (CUtil::GetParentPath(strPath, strParentPath))
    {
      for (int i = 0; i < (int)items.Size(); ++i)
      {
        CFileItemPtr pItem = items[i];
        CUtil::RemoveSlashAtEnd(pItem->m_strPath);
        if (pItem->m_strPath == strPath)
        {
          CStdString strHistory;
          GetDirectoryHistoryString(pItem.get(), strHistory);
          m_history.SetSelectedItem(strHistory, "");
          CUtil::AddSlashAtEnd(strPath);
          m_history.AddPathFront(strPath);
          m_history.AddPathFront("");

          //m_history.DumpPathHistory();
          return ;
        }
      }

      CUtil::AddSlashAtEnd(strPath);
      m_history.AddPathFront(strPath);
      m_history.SetSelectedItem(strPath, strParentPath);
      strPath = strParentPath;
      CUtil::RemoveSlashAtEnd(strPath);
    }
  }
  else
    m_history.ClearPathHistory();

  //m_history.DumpPathHistory();
}

// \brief Override if you want to change the default behavior, what is done
// when the user clicks on a file.
// This function is called by OnClick()
bool CGUIMediaWindow::OnPlayMedia(int iItem)
{
  return OnPlayMedia(m_vecItems->Get(iItem).get());
}

bool CGUIMediaWindow::OnPlayMedia(CFileItem* pItem)
{
  // Reset Playlistplayer, playback started now does
  // not use the playlistplayer.
  g_playlistPlayer.Reset();
  g_playlistPlayer.SetCurrentPlaylist(PLAYLIST_NONE);
  
  bool bResult = false;
  if (pItem->IsInternetStream() || pItem->IsPlayList())
    bResult = g_application.PlayMedia(*pItem, m_guiState->GetPlaylist());
  else
    bResult = g_application.PlayFile(*pItem);
  
  if (pItem->m_lStartOffset == STARTOFFSET_RESUME)
    pItem->m_lStartOffset = 0;
  
  return bResult;
}

// \brief Synchonize the fileitems with the playlistplayer
// It recreated the playlist of the playlistplayer based
// on the fileitems of the window
void CGUIMediaWindow::UpdateFileList()
{
  int nItem = m_viewControl.GetSelectedItem();
  CStdString strSelected;
  if (nItem >= 0)
    strSelected = m_vecItems->Get(nItem)->m_strPath;

  FormatAndSort(*m_vecItems);
  UpdateButtons();

  m_viewControl.SetItems(*m_vecItems);
  m_viewControl.SetSelectedItem(strSelected);

  //  set the currently playing item as selected, if its in this directory
  if (m_guiState.get() && m_guiState->IsCurrentPlaylistDirectory(m_vecItems->m_strPath))
  {
    int iPlaylist=m_guiState->GetPlaylist();
    int nSong = g_playlistPlayer.GetCurrentSong();
    CFileItem playlistItem;
    if (nSong > -1 && iPlaylist > -1)
      playlistItem=*g_playlistPlayer.GetPlaylist(iPlaylist)[nSong];

    g_playlistPlayer.ClearPlaylist(iPlaylist);
    g_playlistPlayer.Reset();

    for (int i = 0; i < m_vecItems->Size(); i++)
    {
      CFileItemPtr pItem = m_vecItems->Get(i);
      if (pItem->m_bIsFolder)
        continue;

      if (!pItem->IsPlayList() && !pItem->IsZIP() && !pItem->IsRAR())
        g_playlistPlayer.Add(iPlaylist, pItem);

      if (pItem->m_strPath == playlistItem.m_strPath &&
          pItem->m_lStartOffset == playlistItem.m_lStartOffset)
        g_playlistPlayer.SetCurrentSong(g_playlistPlayer.GetPlaylist(iPlaylist).size() - 1);
    }
  }
}

void CGUIMediaWindow::OnDeleteItem(int iItem)
{
  if ( iItem < 0 || iItem >= m_vecItems->Size()) return;
  CFileItemPtr item = m_vecItems->Get(iItem);

  // Confirm.
  if (!CGUIDialogYesNo::ShowAndGetInput(122, 125, 0, 0))
    return;
  
  // Delete.
  CStdString strData;
  CFileCurl http;
  bool status = http.Delete(item->GetProperty("key"), strData);
  
  if (status == false)
  {
    // Show error.
    CGUIDialogOK::ShowAndGetInput(257, 16205, 0, 0);
  }
  else
  {
    // Refresh.
    g_directoryCache.ClearDirectory(m_vecItems->m_strPath);
    Update(m_vecItems->m_strPath);
    m_viewControl.SetSelectedItem(iItem);
  }
}

void CGUIMediaWindow::OnRenameItem(int iItem)
{
  if ( iItem < 0 || iItem >= m_vecItems->Size()) return;

  if (g_settings.GetCurrentProfile().getLockMode() != LOCK_MODE_EVERYONE && g_settings.GetCurrentProfile().filesLocked())
    if (!g_passwordManager.IsMasterLockUnlocked(true))
      return;

  if (!CFileUtils::RenameFile(m_vecItems->Get(iItem)->m_strPath))
    return;
  m_vecItems->RemoveDiscCache(GetID());
  Update(m_vecItems->m_strPath);
  m_viewControl.SetSelectedItem(iItem);
}

void CGUIMediaWindow::OnInitWindow()
{
  // initial fetch is done unthreaded to ensure the items are setup prior to skin animations kicking off
  m_rootDir.SetAllowThreads(false);
  Update(m_vecItems->m_strPath);
  m_rootDir.SetAllowThreads(true);

  if (m_iSelectedItem > -1)
    m_viewControl.SetSelectedItem(m_iSelectedItem);

  CGUIWindow::OnInitWindow();
}

CGUIControl *CGUIMediaWindow::GetFirstFocusableControl(int id)
{
  if (m_viewControl.HasControl(id))
    id = m_viewControl.GetCurrentControl();
  return CGUIWindow::GetFirstFocusableControl(id);
}

void CGUIMediaWindow::SetupShares()
{
  // Setup shares and filemasks for this window
  CFileItemList items;
  CGUIViewState* viewState=CGUIViewState::GetViewState(GetID(), items);
  if (viewState)
  {
    m_rootDir.SetMask(viewState->GetExtensions());
    m_rootDir.SetSources(viewState->GetSources());
    delete viewState;
  }
}

bool CGUIMediaWindow::OnPopupMenu(int iItem)
{
  // popup the context menu
  // grab our context menu
  CContextButtons buttons;
  GetContextButtons(iItem, buttons);

  if (buttons.size())
  {
    // mark the item
    if (iItem >= 0 && iItem < m_vecItems->Size())
      m_vecItems->Get(iItem)->Select(true);

    int choice = CGUIDialogContextMenu::ShowAndGetChoice(buttons);

    // deselect our item
    if (iItem >= 0 && iItem < m_vecItems->Size())
      m_vecItems->Get(iItem)->Select(false);

    if (choice >= 0)
      return OnContextButton(iItem, (CONTEXT_BUTTON)choice);
  }
  return false;
}

void CGUIMediaWindow::GetContextButtons(int itemNumber, CContextButtons &buttons)
{
  CFileItemPtr item = (itemNumber >= 0 && itemNumber < m_vecItems->Size()) ? m_vecItems->Get(itemNumber) : CFileItemPtr();

  if (!item)
    return;
  
  // add rating options
  if (item->HasProperty("ratingKey") && item->HasProperty("pluginIdentifier") && (item->IsRemoteSharedPlexMediaServerLibrary() == false))
    buttons.Add(CONTEXT_BUTTON_RATING, item->HasProperty("userRating") ? 40206 : 40205);

  if (item->IsPlexMediaServerLibrary() && 
      (item->IsRemoteSharedPlexMediaServerLibrary() == false) &&
      (item->GetProperty("type") == "episode" || item->GetProperty("type") == "movie" || 
       item->GetProperty("type") == "track"   || item->GetProperty("type") == "photo"))
  {
    buttons.Add(CONTEXT_BUTTON_DELETE, 15015);
  }
  
  // user added buttons
  CStdString label;
  CStdString action;
  for (int i = CONTEXT_BUTTON_USER1; i <= CONTEXT_BUTTON_USER10; i++)
  {
    label.Format("contextmenulabel(%i)", i - CONTEXT_BUTTON_USER1);
    if (item->GetProperty(label).IsEmpty())
      break;

    action.Format("contextmenuaction(%i)", i - CONTEXT_BUTTON_USER1);
    if (item->GetProperty(action).IsEmpty())
      break;

    buttons.Add((CONTEXT_BUTTON)i, item->GetProperty(label));
  }
}

bool CGUIMediaWindow::OnContextButton(int itemNumber, CONTEXT_BUTTON button)
{
  switch (button)
  {
  case CONTEXT_BUTTON_PLUGIN_SETTINGS:
    {
      CURL plugin(m_vecItems->Get(itemNumber)->m_strPath);
      ADDON::AddonPtr addon;
      if (CAddonMgr::Get().GetAddon(plugin.GetHostName(), addon, ADDON_PLUGIN))
        if (CGUIDialogAddonSettings::ShowAndGetInput(addon))
          Update(m_vecItems->m_strPath);
      return true;
    }
  case CONTEXT_BUTTON_DELETE:
  {
    OnDeleteItem(itemNumber);
    return true;
  }
  case CONTEXT_BUTTON_RATING:
  {
    CFileItemPtr item = m_vecItems->Get(itemNumber);
    
    bool hasUserRating = item->HasProperty("userRating");
    int newRating = CGUIDialogRating::ShowAndGetInput(hasUserRating ? 40208 : 40207,
                                                      item->GetVideoInfoTag()->m_strTitle,
                                                      hasUserRating? item->GetPropertyInt("userRating") : (int)item->GetVideoInfoTag()->m_fRating);
    
    if (newRating >= 0 && newRating <= 10)
    {
      PlexMediaServerQueue::Get().onRate(item, float(newRating));
      item->SetProperty("userRating", newRating);
    }
    
    return true;
  }
  case CONTEXT_BUTTON_USER1:
  case CONTEXT_BUTTON_USER2:
  case CONTEXT_BUTTON_USER3:
  case CONTEXT_BUTTON_USER4:
  case CONTEXT_BUTTON_USER5:
  case CONTEXT_BUTTON_USER6:
  case CONTEXT_BUTTON_USER7:
  case CONTEXT_BUTTON_USER8:
  case CONTEXT_BUTTON_USER9:
  case CONTEXT_BUTTON_USER10:
    {
      CStdString action;
      action.Format("contextmenuaction(%i)", button - CONTEXT_BUTTON_USER1);
      g_application.getApplicationMessenger().ExecBuiltIn(m_vecItems->Get(itemNumber)->GetProperty(action));
      return true;
    }
  default:
    break;
  }
  return false;
}

const CGUIViewState *CGUIMediaWindow::GetViewState() const
{
  return m_guiState.get();
}

const CFileItemList& CGUIMediaWindow::CurrentDirectory() const
{
  return *m_vecItems;
}

bool CGUIMediaWindow::WaitForNetwork() const
{
  if (g_application.getNetwork().IsAvailable())
    return true;

  CGUIDialogProgress *progress = (CGUIDialogProgress *)g_windowManager.GetWindow(WINDOW_DIALOG_PROGRESS);
  if (!progress)
    return true;

  CURL url(m_vecItems->m_strPath);
  progress->SetHeading(1040); // Loading Directory
  progress->SetLine(1, url.GetWithoutUserDetails());
  progress->ShowProgressBar(false);
  progress->StartModal();
  while (!g_application.getNetwork().IsAvailable())
  {
    progress->Progress();
    if (progress->IsCanceled())
    {
      progress->Close();
      return false;
    }
  }
  progress->Close();
  return true;
}

void CGUIMediaWindow::OnFilterItems(const CStdString &filter)
{
  CStdString currentItem;
  int item = m_viewControl.GetSelectedItem();
  if (item >= 0)
    currentItem = m_vecItems->Get(item)->m_strPath;
  
  m_viewControl.Clear();
  
  CFileItemList items;
  GetFilteredItems(filter, items);
  if (filter.IsEmpty() || items.GetObjectCount() > 0)
  {
    m_vecItems->ClearItems();
    m_vecItems->Append(items);
    SetProperty("filter", filter);
  }
  
  // and update our view control + buttons
  m_viewControl.SetItems(*m_vecItems);
  m_viewControl.SetSelectedItem(currentItem);
  UpdateButtons();
}

void CGUIMediaWindow::GetFilteredItems(const CStdString &filter, CFileItemList &items)
{
  CStdString trimmedFilter(filter);
  trimmedFilter.TrimLeft().ToLower();
  
  if (trimmedFilter.IsEmpty())
  {
    items.Append(*m_unfilteredItems);
    return;
  }
  
  bool numericMatch = StringUtils::IsNaturalNumber(trimmedFilter);
  for (int i = 0; i < m_unfilteredItems->Size(); i++)
  {
    CFileItemPtr item = m_unfilteredItems->Get(i);
    if (item->IsParentFolder())
    {
      items.Add(item);
      continue;
    }
    // TODO: Need to update this to get all labels, ideally out of the displayed info (ie from m_layout and m_focusedLayout)
    // though that isn't practical.  Perhaps a better idea would be to just grab the info that we should filter on based on
    // where we are in the library tree.
    // Another idea is tying the filter string to the current level of the tree, so that going deeper disables the filter,
    // but it's re-enabled on the way back out.
    CStdString match;
    /*    if (item->GetFocusedLayout())
     match = item->GetFocusedLayout()->GetAllText();
     else if (item->GetLayout())
     match = item->GetLayout()->GetAllText();
     else*/
    match = item->GetLabel(); // Filter label only for now
    
    if (numericMatch)
      StringUtils::WordToDigits(match);
    
    size_t pos = StringUtils::FindWords(match.c_str(), trimmedFilter.c_str());
    if (pos != CStdString::npos)
      items.Add(item);
  }
}

CStdString CGUIMediaWindow::GetStartFolder(const CStdString &dir)
{
  if (dir.Equals("$ROOT") || dir.Equals("Root"))
    return "";
  return dir;
}

void CGUIMediaWindow::Render()
{
  if (m_refreshTimer.IsRunning() && m_vecItems->m_autoRefresh > 0 && m_refreshTimer.GetElapsedSeconds() >= m_vecItems->m_autoRefresh)
  {
    if (m_mediaRefresher == NULL)
    {
      // Start the directory auto-refreshing.
      m_mediaRefresher = new MediaRefresher(m_vecItems->m_strPath);
    }
    else if (m_mediaRefresher->isDone())
    {
      // Assign the new stuff over.
      m_vecItems->ClearItems();
      m_vecItems->Append(m_mediaRefresher->getItemList());
      m_vecItems->m_autoRefresh = m_mediaRefresher->getItemList().m_autoRefresh;
      
      OnPrepareFileItems(*m_vecItems);
      m_vecItems->FillInDefaultIcons();
      FormatAndSort(*m_vecItems);
      OnFinalizeFileItems(*m_vecItems);
      m_viewControl.SetItems(*m_vecItems);
      
      // Thumbnails.
      if (GetBackgroundLoader())
      {
        if (GetBackgroundLoader()->IsLoading())
          GetBackgroundLoader()->StopThread();
        
        GetBackgroundLoader()->Load(*m_vecItems);
      }
      
      // Whack the timer.
      m_refreshTimer.Reset();
      m_mediaRefresher->die();
      m_mediaRefresher = NULL;
    }
  }
  
  CGUIWindow::Render();
}
