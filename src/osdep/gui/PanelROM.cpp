#ifdef USE_SDL2
#include <guisan.hpp>
#include <SDL_ttf.h>
#include <guisan/sdl.hpp>
#include <guisan/sdl/sdltruetypefont.hpp>
#else
#include <guichan.hpp>
#include <SDL/SDL_ttf.h>
#include <guichan/sdl.hpp>
#include "sdltruetypefont.hpp"
#endif
#include "SelectorEntry.hpp"
#include "UaeDropDown.hpp"

#include "sysconfig.h"
#include "sysdeps.h"
#include "config.h"
#include "options.h"
#include "include/memory-uae.h"
#include "rommgr.h"
#include "uae.h"
#include "gui.h"
#include "gui_handling.h"
#include "GenericListModel.h"

static gcn::Label *lblMainROM;
static gcn::UaeDropDown* cboMainROM;
static gcn::Button *cmdMainROM;
static gcn::Label *lblExtROM;
static gcn::UaeDropDown* cboExtROM;
static gcn::Button *cmdExtROM;
#ifdef ACTION_REPLAY
static gcn::Label *lblCartROM;
static gcn::UaeDropDown* cboCartROM;
static gcn::Button *cmdCartROM;
#endif
static gcn::Label *lblUAEROM;
static gcn::UaeDropDown* cboUAEROM;


class ROMListModel : public gcn::ListModel
{
  private:
    std::vector<std::string> roms;
    std::vector<int> idxToAvailableROMs;
    int ROMType;
    
  public:
    ROMListModel(int romtype)
    {
      ROMType = romtype;
    }
    
    int getNumberOfElements()
    {
      return roms.size();
    }

    std::string getElementAt(int i)
    {
      if(i < 0 || i >= roms.size())
        return "---";
      return roms[i];
    }
    
    AvailableROM* getROMat(int i)
    {
      if(i >= 0 && i < idxToAvailableROMs.size())
        return idxToAvailableROMs[i] < 0 ? NULL : lstAvailableROMs[idxToAvailableROMs[i]];
      return NULL;
    }
    
    int InitROMList(char *current)
    {
      roms.clear();
      idxToAvailableROMs.clear();
      
      int currIdx = -1;
      if(ROMType & (ROMTYPE_ALL_EXT | ROMTYPE_ALL_CART))
      {
        roms.push_back(" "); 
        idxToAvailableROMs.push_back(-1);
        currIdx = 0;
      }
      for(int i=0; i<lstAvailableROMs.size(); ++i)
      {
        if(lstAvailableROMs[i]->ROMType & ROMType)
        {
          if(!strcasecmp(lstAvailableROMs[i]->Path, current))
            currIdx = roms.size();
          roms.push_back(lstAvailableROMs[i]->Name);
          idxToAvailableROMs.push_back(i);
        }
      }
      return currIdx;
    }
};
static ROMListModel *mainROMList;
static ROMListModel *extROMList;
#ifdef ACTION_REPLAY
static ROMListModel *cartROMList;
#endif

static const TCHAR* uaeList[] = { _T("ROM disabled"), _T("Original UAE (FS + F0 ROM)"), _T("New UAE (64k + F0 ROM)") };
static gcn::GenericListModel uaeROMList(uaeList, 3);


void RefreshPanelROM(void)
{
  int idx = mainROMList->InitROMList(workprefs.romfile);
  cboMainROM->setSelected(idx);
  
  idx = extROMList->InitROMList(workprefs.romextfile);
  cboExtROM->setSelected(idx);
  
#ifdef ACTION_REPLAY
  idx = cartROMList->InitROMList(workprefs.cartfile);
  cboCartROM->setSelected(idx);
#endif

	if (workprefs.boot_rom == 1) {
		cboUAEROM->setSelected(0);
	} else {
		cboUAEROM->setSelected(workprefs.uaeboard + 1);
	}
	cboUAEROM->setEnabled(!emulating);
}


class ROMActionListener : public gcn::ActionListener
{
  public:
    void action(const gcn::ActionEvent& actionEvent)
    {
      char tmp[MAX_PATH];
      const char *filter[] = { ".rom", "\0" };

      if (actionEvent.getSource() == cboMainROM) {
        AvailableROM* rom = mainROMList->getROMat(cboMainROM->getSelected());
        if(rom != NULL)
          strncpy(workprefs.romfile, rom->Path, sizeof(workprefs.romfile) - 1);

      } else if (actionEvent.getSource() == cboExtROM) {
        AvailableROM* rom = extROMList->getROMat(cboExtROM->getSelected());
        if(rom != NULL)
          strncpy(workprefs.romextfile, rom->Path, sizeof(workprefs.romextfile) - 1);
        else
          strncpy(workprefs.romextfile, " ", sizeof(workprefs.romextfile) - 1);

#ifdef ACTION_REPLAY
      } else if (actionEvent.getSource() == cboCartROM) {
        AvailableROM* rom = cartROMList->getROMat(cboCartROM->getSelected());
        if(rom != NULL)
          strncpy(workprefs.cartfile, rom->Path, sizeof(workprefs.cartfile) - 1);
        else
          strncpy(workprefs.cartfile, " ", sizeof(workprefs.cartfile) - 1);
#endif

      } else if (actionEvent.getSource() == cmdMainROM) {
        strncpy(tmp, currentDir, MAX_PATH - 1);
        if(SelectFile("Select System ROM", tmp, filter))
        {
          AvailableROM *newrom;
          newrom = new AvailableROM();
          extractFileName(tmp, newrom->Name);
          removeFileExtension(newrom->Name);
          strncpy(newrom->Path, tmp, MAX_PATH - 1);
          newrom->ROMType = ROMTYPE_KICK;
          lstAvailableROMs.push_back(newrom);
          strncpy(workprefs.romfile, tmp, sizeof(workprefs.romfile) - 1);
          RefreshPanelROM();
        }
        cmdMainROM->requestFocus();

      } else if (actionEvent.getSource() == cmdExtROM) {
        strncpy(tmp, currentDir, MAX_PATH - 1);
        if(SelectFile("Select Extended ROM", tmp, filter))
        {
          AvailableROM *newrom;
          newrom = new AvailableROM();
          extractFileName(tmp, newrom->Name);
          removeFileExtension(newrom->Name);
          strncpy(newrom->Path, tmp, MAX_PATH - 1);
          newrom->ROMType = ROMTYPE_EXTCD32;
          lstAvailableROMs.push_back(newrom);
          strncpy(workprefs.romextfile, tmp, sizeof(workprefs.romextfile) - 1);
          RefreshPanelROM();
        }
        cmdExtROM->requestFocus();

#ifdef ACTION_REPLAY
      } else if (actionEvent.getSource() == cmdCartROM) {
        strncpy(tmp, currentDir, MAX_PATH - 1);
        if(SelectFile("Select Cartridge ROM", tmp, filter))
        {
          AvailableROM *newrom;
          newrom = new AvailableROM();
          extractFileName(tmp, newrom->Name);
          removeFileExtension(newrom->Name);
          strncpy(newrom->Path, tmp, MAX_PATH - 1);
          newrom->ROMType = ROMTYPE_CD32CART;
          lstAvailableROMs.push_back(newrom);
          strncpy(workprefs.romextfile, tmp, sizeof(workprefs.romextfile) - 1);
          RefreshPanelROM();
        }
        cmdCartROM->requestFocus();
#endif

      } else if (actionEvent.getSource() == cboUAEROM) {
        int v = cboUAEROM->getSelected();
      	if (v > 0) {
      		workprefs.uaeboard = v - 1;
      		workprefs.boot_rom = 0;
      	} else {
      		workprefs.uaeboard = 0;
      		workprefs.boot_rom = 1; // disabled
      	}

      }
    }
};
static ROMActionListener* romActionListener;


void InitPanelROM(const struct _ConfigCategory& category)
{
  romActionListener = new ROMActionListener();
  mainROMList = new ROMListModel(ROMTYPE_ALL_KICK);
  extROMList = new ROMListModel(ROMTYPE_ALL_EXT);
#ifdef ACTION_REPLAY
  cartROMList = new ROMListModel(ROMTYPE_ALL_CART);
#endif
  int cboWidth;
#ifdef ANDROID
  cboWidth = 450;
#else
  cboWidth = 400;
#endif
  lblMainROM = new gcn::Label("Main ROM File:");
  lblMainROM->setSize(200, LABEL_HEIGHT);
	cboMainROM = new gcn::UaeDropDown(mainROMList);
  cboMainROM->setSize(cboWidth, DROPDOWN_HEIGHT);
  cboMainROM->setBaseColor(gui_baseCol);
  cboMainROM->setId("cboMainROM");
  cboMainROM->addActionListener(romActionListener);
  cmdMainROM = new gcn::Button("...");
  cmdMainROM->setId("MainROM");
  cmdMainROM->setSize(SMALL_BUTTON_WIDTH, SMALL_BUTTON_HEIGHT);
  cmdMainROM->setBaseColor(gui_baseCol);
  cmdMainROM->addActionListener(romActionListener);

  lblExtROM = new gcn::Label("Extended ROM File:");
  lblExtROM->setSize(200, LABEL_HEIGHT);
	cboExtROM = new gcn::UaeDropDown(extROMList);
  cboExtROM->setSize(cboWidth, DROPDOWN_HEIGHT);
  cboExtROM->setBaseColor(gui_baseCol);
  cboExtROM->setId("cboExtROM");
  cboExtROM->addActionListener(romActionListener);
  cmdExtROM = new gcn::Button("...");
  cmdExtROM->setId("ExtROM");
  cmdExtROM->setSize(SMALL_BUTTON_WIDTH, SMALL_BUTTON_HEIGHT);
  cmdExtROM->setBaseColor(gui_baseCol);
  cmdExtROM->addActionListener(romActionListener);

#ifdef ACTION_REPLAY
  lblCartROM = new gcn::Label("Cartridge ROM File:");
  lblCartROM->setSize(200, LABEL_HEIGHT);
	cboCartROM = new gcn::UaeDropDown(cartROMList);
  cboCartROM->setSize(cboWidth, DROPDOWN_HEIGHT);
  cboCartROM->setBaseColor(gui_baseCol);
  cboCartROM->setId("cboCartROM");
  cboCartROM->addActionListener(romActionListener);
  cmdCartROM = new gcn::Button("...");
  cmdCartROM->setId("CartROM");
  cmdCartROM->setSize(SMALL_BUTTON_WIDTH, SMALL_BUTTON_HEIGHT);
  cmdCartROM->setBaseColor(gui_baseCol);
  cmdCartROM->addActionListener(romActionListener);
#endif

  lblUAEROM = new gcn::Label("Advanced UAE expansion board/Boot ROM:");
  lblUAEROM->setSize(400, LABEL_HEIGHT);
	cboUAEROM = new gcn::UaeDropDown(&uaeROMList);
  cboUAEROM->setSize(400, DROPDOWN_HEIGHT);
  cboUAEROM->setBaseColor(gui_baseCol);
  cboUAEROM->setId("cboUAEROM");
  cboUAEROM->addActionListener(romActionListener);
  
  int posY = DISTANCE_BORDER;
  category.panel->add(lblMainROM, DISTANCE_BORDER, posY);
  posY += lblMainROM->getHeight() + 4;
  category.panel->add(cboMainROM, DISTANCE_BORDER, posY);
  category.panel->add(cmdMainROM, DISTANCE_BORDER + cboMainROM->getWidth() + DISTANCE_NEXT_X, posY);
  posY += cboMainROM->getHeight() + DISTANCE_NEXT_Y;

  category.panel->add(lblExtROM, DISTANCE_BORDER, posY);
  posY += lblExtROM->getHeight() + 4;
  category.panel->add(cboExtROM, DISTANCE_BORDER, posY);
  category.panel->add(cmdExtROM, DISTANCE_BORDER + cboExtROM->getWidth() + DISTANCE_NEXT_X, posY);
  posY += cboExtROM->getHeight() + DISTANCE_NEXT_Y;
  
#ifdef ACTION_REPLAY


  category.panel->add(lblCartROM, DISTANCE_BORDER, posY);
  posY += lblCartROM->getHeight() + 4;
  category.panel->add(cboCartROM, DISTANCE_BORDER, posY);
  category.panel->add(cmdCartROM, DISTANCE_BORDER + cboCartROM->getWidth() + DISTANCE_NEXT_X, posY);
  posY += cboCartROM->getHeight() + DISTANCE_NEXT_Y;
#endif

  category.panel->add(lblUAEROM, DISTANCE_BORDER, posY);
  posY += lblUAEROM->getHeight() + 4;
  category.panel->add(cboUAEROM, DISTANCE_BORDER, posY);
  posY += cboUAEROM->getHeight() + DISTANCE_NEXT_Y;
  
  RefreshPanelROM();
}


void ExitPanelROM(const struct _ConfigCategory& category)
{
  category.panel->clear();
  
  delete lblMainROM;
  delete cboMainROM;
  delete cmdMainROM;
  delete mainROMList;
  
  delete lblExtROM;
  delete cboExtROM;
  delete cmdExtROM;
  delete extROMList;

#ifdef ACTION_REPLAY
  delete lblCartROM;
  delete cboCartROM;
  delete cmdCartROM;
  delete cartROMList;
#endif

  delete lblUAEROM;
  delete cboUAEROM;
  
  delete romActionListener;
}


bool HelpPanelROM(std::vector<std::string> &helptext)
{
  helptext.clear();
  helptext.push_back("Select the required kickstart ROM for the Amiga you want to emulate in \"Main ROM File\".");
  helptext.push_back(" ");
  helptext.push_back("In \"Extended ROM File\", you can only select the required ROM for CD32 emulation.");
  helptext.push_back(" ");
  helptext.push_back("In \"Cartridge ROM File\", you can select the CD32 FMV module to activate video playback in CD32.");
  helptext.push_back("There are also some Action Replay and Freezer cards and the built in HRTMon available.");
  return true;
}
