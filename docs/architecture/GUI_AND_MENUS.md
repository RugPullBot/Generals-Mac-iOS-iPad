# GUI and Menus Architecture

Scope: the retained-mode windowing system that draws every menu, popup and in-game panel —
`GameWindow`, `GameWindowManager`, the `.wnd` layout parser, the gadget controls
(`Gadget*`), name-based callback binding through `FunctionLexicon`, `WindowLayout`, and the
`Shell` screen stack. Includes a dedicated section on **creating controls at runtime without
touching `.wnd` files**, because the `.wnd` editor is Windows-only and cannot be run on this
port.

Everything below was read from the tree; every non-obvious claim cites `file:line`. Line
numbers are accurate at the time of writing — if one looks off by a few lines, the enclosing
function name is the reliable anchor.

Unless noted otherwise, paths are relative to the repo root. Note that the class definitions
live in `Core/` but `GameWindowManager` itself is **per-game**: the Zero Hour copy in
`GeneralsMD/Code/GameEngine/Source/GameClient/GUI/` is the one that builds into `z_generals`.

---

## 1. How it fits together (read this first)

```
                    .wnd file  (Data/Window/Menus/*.wnd, usually inside a .big)
                         |
                         |  Shell::push("Menus/MapSelectMenu.wnd")        Shell.cpp:320
                         v
        Shell::doPush -> GameWindowManager::winCreateLayout()             Shell.cpp:694
                         |
                         v
        WindowLayout::load -> winCreateFromScript()                       WindowLayout.cpp:206
                         |
        +----------------+-----------------------------------------------+
        |                                                                 |
   parseLayoutBlock                                              repeated parseWindow()
   (LAYOUTINIT/UPDATE/SHUTDOWN)                                  GameWindowManagerScript.cpp:2312
   names -> FunctionLexicon                                              |
   GameWindowManagerScript.cpp:2578                                      v
        |                                                       createWindow()  :2018
        |                                                          |         |
        |                                        "USER"/"TABPANE"  |         |  gadget types
        |                                        winCreate()       |         v
        |                                                          |    createGadget()  :1616
        |                                                          |         |
        |                                                          v         v
        |                                            TheWindowManager->gogoGadget*(...)
        |                                            GameWindowManager.cpp:1794..2818
        v
   WindowLayout { m_init, m_update, m_shutdown, list of ROOT windows }
        |
        v
   Shell::m_screenStack[]  (max 16)                                        Shell.h:171-173
        |
        +-- Shell::update()  runs runUpdate() on EVERY layout on the stack  Shell.cpp:192-198
        |
   TheWindowManager->m_windowList  (flat list of ALL top-level windows, all layouts)
        |
        +-- input:  WindowXlat -> winProcessMouseEvent / winProcessKey      WindowXlat.cpp:242,315
        +-- draw:   W3DInGameUI::draw -> winRepaint()                       W3DInGameUI.cpp:435
        +-- tick:   GameClient::update -> TheWindowManager->UPDATE()        GameClient.cpp:647
```

Four things are worth internalising immediately:

1. **There are two parallel lists.** `GameWindowManager::m_windowList` is the flat, z-ordered
   list of *all* top-level windows in the process (`GameWindowManager.cpp:343`). `WindowLayout`
   keeps its own separate doubly-linked list threaded through `m_nextLayout`/`m_prevLayout`
   (`GameWindow.h:426-428`). A window can be on both. Destroying a layout walks the *layout*
   list and calls `winDestroy` on each root (`WindowLayout.cpp:168-183`).

2. **Window IDs are `NameKey`s of decorated strings.** The `.wnd` `NAME = "File.wnd:Control"`
   field is hashed to a `NameKeyType` and stored as the window ID
   (`GameWindowManagerScript.cpp:647-652`). All menu code looks controls up with
   `TheWindowManager->winGetWindowFromId(nullptr, NAMEKEY("File.wnd:Control"))`.

3. **Callbacks are bound by name at load time**, resolved through the `FunctionLexicon`
   string→function-pointer tables (`GameWindowManagerScript.cpp:707`). A callback that is not
   in a lexicon table silently binds to `nullptr`.

4. **Gadgets talk to their *owner*, not their parent.** A push button sends `GBM_SELECTED` to
   `instData->getOwner()` (`GadgetPushButton.cpp:256`), which is set to the creating parent at
   construction (`GameWindowManager.cpp:1841`). Menu logic therefore lives in the root
   window's *system* callback, dispatching on `control->winGetWindowId()`.

### Where the code lives

| Path | What |
| --- | --- |
| `Core/GameEngine/Include/GameClient/GameWindow.h` | `GameWindow`, message enums, status bits, callback typedefs |
| `Core/GameEngine/Source/GameClient/GUI/GameWindow.cpp` | `GameWindow` methods, default (no-op) callbacks |
| `Core/GameEngine/Include/GameClient/Gadget.h` | `GWS_*` styles, `G*M_*` messages, per-gadget data structs |
| `Core/GameEngine/Include/GameClient/Gadget{PushButton,ListBox,ComboBox,TextEntry,Slider,StaticText,CheckBox,RadioButton,ProgressBar,TabControl}.h` | the `Gadget*` helper APIs |
| `Core/GameEngine/Source/GameClient/GUI/Gadget/*.cpp` | gadget system/input callbacks and helper implementations |
| `Core/GameEngine/Source/GameClient/GUI/WindowLayout.cpp` | `WindowLayout` |
| `Core/GameEngine/Include/GameClient/WinInstanceData.h` | `WinInstanceData` — the per-window "properties bag" |
| `GeneralsMD/Code/GameEngine/Include/GameClient/GameWindowManager.h` | manager interface, `gogoGadget*` factory methods |
| `GeneralsMD/.../GUI/GameWindowManager.cpp` | manager implementation (4100 lines) |
| `GeneralsMD/.../GUI/GameWindowManagerScript.cpp` | the entire `.wnd` parser (2885 lines) |
| `GeneralsMD/.../GUI/Shell/Shell.cpp` | screen stack |
| `GeneralsMD/.../GUI/GUICallbacks/Menus/*.cpp` | one file per menu screen (44 of them) |
| `GeneralsMD/.../Common/System/FunctionLexicon.cpp` | device-independent name→function tables |
| `GeneralsMD/Code/GameEngineDevice/.../W3DFunctionLexicon.cpp` | device (W3D) draw/init tables |
| `GeneralsMD/Code/GameEngineDevice/.../W3DGameWindowManager.h` | concrete manager; supplies the real draw funcs |
| `GeneralsMD/.../GUI/ControlBar/` | the in-game command bar (separate subsystem, same window primitives) |

---

## 2. The `.wnd` file format and its parser

### 2.1 What a file looks like

The only `.wnd` checked into this repo is `GeneralsZH/Data/Window/Menus/ExtrasMenu.wnd` (a
GeneralsX addition — stock Zero Hour has no such file; see `Shell.cpp:700-705`). Abbreviated:

```
FILE_VERSION = 2;
STARTLAYOUTBLOCK
  LAYOUTINIT = ExtrasMenuInit;
  LAYOUTUPDATE = ExtrasMenuUpdate;
  LAYOUTSHUTDOWN = ExtrasMenuShutdown;
ENDLAYOUTBLOCK
WINDOW
  WINDOWTYPE = USER;
  SCREENRECT = UPPERLEFT: 0 0, BOTTOMRIGHT: 800 600, CREATIONRESOLUTION: 800 600;
  NAME = "ExtrasMenu.wnd:ExtrasMenuParent";
  STATUS = ENABLED+IMAGE+NOFOCUS;
  STYLE = USER;
  SYSTEMCALLBACK = "ExtrasMenuSystem";
  INPUTCALLBACK = "ExtrasMenuInput";
  TOOLTIPCALLBACK = "[None]";
  DRAWCALLBACK = "[None]";
  FONT = NAME: "Arial", SIZE: 10, BOLD: 0;
  ENABLEDDRAWDATA = IMAGE: NoImage, COLOR: 2 2 2 193, BORDERCOLOR: ... ;  (9 triples)
  DISABLEDDRAWDATA = ... ;
  HILITEDRAWDATA  = ... ;
  CHILD
  WINDOW
    WINDOWTYPE = STATICTEXT;
    ...
  END
  ENDALLCHILDREN
END
```

Grammar notes drawn from the parser:

- `WINDOWTYPE` and `SCREENRECT` are **positionally required** as the first two fields of every
  `WINDOW` block — the parser `assert`s on the token names
  (`GameWindowManagerScript.cpp:2371, 2384`) rather than searching for them.
- Everything after that is a table-driven dispatch over `gameWindowFieldList`
  (`:2254-2310`), so field order is free. Unrecognised field names are silently skipped by
  eating up to the next `;` (`:2477-2483`).
- `CHILD` … `ENDALLCHILDREN` nests. `parseChildWindows` (`:2140`) pushes the current window
  onto a static parent stack of depth 10 (`WIN_STACK_DEPTH`, `:86`).
- Each `WINDOW` block terminates with `END`; `parseWindow` creates the window at that point
  if it hasn't already (`:2439-2453`).
- `FILE_VERSION` must be the very first thing; the parser literally skips
  `strlen("FILE_VERSION = ")` bytes and then scans an int (`:2742-2744`). Version ≥ 2 gets a
  `STARTLAYOUTBLOCK`; version 1 files get `"[None]"` layout callbacks (`:2747-2767`).

### 2.2 Coordinate scaling — this is not a fixed-resolution UI

`parseScreenRect` reads `CREATIONRESOLUTION` and then **non-uniformly scales** the rect by the
current display size (`GameWindowManagerScript.cpp:528-533`):

```c
Real xScale = (Real)TheDisplay->getWidth()  / (Real)createRes.x;
Real yScale = (Real)TheDisplay->getHeight() / (Real)createRes.y;
```

Consequences that matter for this port:

- All stock layouts are authored at 800×600 (4:3). On a 16:9 display everything is stretched
  horizontally; there is no letterboxing, no aspect preservation, no anchoring.
- Scaling happens **at parse time only**. Changing resolution requires rebuilding every
  layout, which is what `Shell::recreateWindowLayouts()` (`Shell.cpp:232-265`) does — it
  records the filenames + hidden flags of the stack, tears the whole shell down, and re-pushes.
- Child positions are converted to parent-relative by subtracting the parent's *screen*
  position (`:541-551`), so a child's `SCREENRECT` in the file is in absolute screen space.

### 2.3 Status and style bit strings

`STATUS = A+B+C` and `STYLE = A+B` are parsed by index into two string tables
(`GameWindowManagerScript.cpp:140-155`), i.e. `WindowStatusNames[i]` maps to bit `1 << i`
(`parseBitFlag`, `:208-215`). The tables must stay in lockstep with the `WIN_STATUS_*`
(`GameWindow.h:156-190`) and `GWS_*` (`Gadget.h:88-122`) enums — the headers say so in
comments, and nothing enforces it.

An unknown flag is **not** an error: `parseBitString` logs and continues (`:241-244`). The
hand-written `ExtrasMenu.wnd` in-tree uses `STYLE = STATICTEXT+RIGHT+VCENTER+USER;`, and
`RIGHT`/`VCENTER` are not in `WindowStyleNames` — they are silently discarded.

Also note `WIN_STATUS_SHORTCUT_BUTTON` (`GameWindow.h:187`) has its name commented out of the
table (`:146`), so it can be set in code but never from a `.wnd` file.

### 2.4 Callback fields

`SYSTEMCALLBACK`, `INPUTCALLBACK`, `TOOLTIPCALLBACK`, `DRAWCALLBACK` all follow the same
shape (`GameWindowManagerScript.cpp:689-792`): strip to the first `"`, take the quoted token,
hash it with `TheNameKeyGenerator`, and look it up in the appropriate `FunctionLexicon` table.

`"[None]"` is not special-cased anywhere. It just hashes to a key that no table contains, and
`findFunction` returns `nullptr`, which `createWindow` treats as "leave the default"
(`:2094-2104`). The same is true for a **typo**: `nameToKey` *creates* a new key on a miss
(`NameKeyGenerator.cpp:148-162`) rather than returning `NAMEKEY_INVALID`, so a misspelled
callback name silently produces a window with no handler and no diagnostic.

### 2.5 Text is a string-table label, not literal text

`TEXT = "EXTRA OPTIONS"` stores an *AsciiString label* into
`instData.m_textLabelString` (`:1049`). At creation, `setWindowText` runs it through
`TheGameText->fetch()` (`:1593`) and routes it to the right gadget setter by style
(`:1595-1609`). A label with no CSF entry renders as `MISSING: 'EXTRA OPTIONS'`
(`Core/GameEngine/Source/GameClient/GameText.cpp:1399-1408`).

There is an inconsistency here worth knowing: `gogoGadgetPushButton` *also* sets the button
text from the label, but via `winTextLabelToText` (`GameWindowManager.cpp:1850-1852`), which
is a plain ASCII→Unicode transliteration with a `@todo` attached
(`GameWindowManager.cpp:3554-3566`) — **no** string-table lookup. In the `.wnd` path the
later `setWindowText` call overwrites it. If you create a push button at runtime, you get the
raw label string as the visible text unless you call `GadgetButtonSetText` yourself.

### 2.6 The parser is a single global state machine

All of the parser's working state is file-static: the parent stack (`:158-159`), the default
colours and font (`:128-134`), the four pending callback pointers and their name strings
(`:118-125`), the per-gadget data templates (`getDataTemplate`, `:1364-1425`), and ~18 arrays
of sub-control draw data (`:163-186`). `winCreateFromScript` resets the stack and defaults at
entry (`:2714-2715`).

This means **`winCreateFromScript` is not reentrant**. Do not call it (directly or via
`winCreateLayout` / `Shell::push`) from inside a window callback that is itself running during
a load. In practice nothing does, but nothing prevents it either.

`parseData` (`:1447`) — the old space-separated `DATA =` form — is dead: the comment at
`:1444` says "THIS FUNCTION IS NEVER REACHED; IT IS OBSOLETE -MDC" and all modern files use the
named `LISTBOXDATA`/`SLIDERDATA`/etc. fields instead.

### 2.7 File lookup

`winCreateFromScript` prefixes `Window\` when the name contains no backslash
(`:2724-2727`), so `Shell::push("Menus/MainMenu.wnd")` resolves `Window\Menus/MainMenu.wnd` —
mixed separators. On non-Windows, `StdLocalFileSystem` normalises `\` to `/` and then does a
case-insensitive fallback search
(`Core/GameEngineDevice/Source/StdDevice/Common/StdLocalFileSystem.cpp:58-75`), so this works,
but the mixed path shows up verbatim in logs.

---

## 3. `GameWindow` and `GameWindowManager`

### 3.1 The window object

`GameWindow` (`Core/GameEngine/Include/GameClient/GameWindow.h:231`) is an abstract
`MemoryPoolObject`; the concrete class is `W3DGameWindow`, allocated by
`W3DGameWindowManager::allocateNewWindow()`
(`GeneralsMD/Code/GameEngineDevice/Include/W3DDevice/GameClient/W3DGameWindowManager.h:83`).
A headless variant (`GameWindowDummy` / `GameWindowManagerDummy`) exists for the
`-headless` mode (`GameWindow.h:440-446`, `GameWindowManager.h:384`).

State per window (`GameWindow.h:400-434`):

| Member | Notes |
| --- | --- |
| `m_status` | the authoritative `WIN_STATUS_*` bitfield |
| `m_region`, `m_size` | `m_region.lo` is the origin, **relative to the parent** |
| `m_instData` | a full `WinInstanceData` by value (images, colours, fonts, id, style, text) |
| `m_userData` | gadget-specific heap struct (`ListboxData*`, `SliderData*`, …) |
| `m_input/m_system/m_draw/m_tooltip` | the four callbacks |
| `m_next/m_prev/m_parent/m_child` | sibling/hierarchy links |
| `m_nextLayout/m_prevLayout/m_layout` | layout links — **root windows only** (`:421-425`) |

The four callbacks default to no-ops in the constructor
(`GameWindow.cpp:101-107`) — but `getDefaultDraw()` is virtual and the W3D manager overrides
it to `W3DGameWinDefaultDraw` (`W3DGameWindowManager.h:84`), so a plain `USER` window does get
a real background/border draw. The tooltip func deliberately defaults to `nullptr`
(`GameWindow.cpp:104-107`).

### 3.2 Creating a window

`GameWindowManager::winCreate` (`GameWindowManager.cpp:1330`) is the single allocation point:

```
allocateNewWindow()
addWindowToParent(window,parent)  or  linkWindow(window)      :1359-1362
m_status / m_size / m_region  <- arguments                    :1364-1373
winSetSystemFunc(system); winSendSystemMsg(GWM_CREATE)        :1376-1377
if (instData) winSetInstanceData(instData)                    :1380-1381
winSetFont(<localized default or "Times New Roman" 14>)       :1385-1407
```

Two ordering facts follow from this:

- `GWM_CREATE` is delivered **before** the instance data is copied in, so a system callback
  cannot read `winGetInstanceData()` fields during `GWM_CREATE` and expect the caller's values.
  (Menu code uses `GWM_CREATE` only to cache `NameKey`s — see `MapSelectMenu.cpp:312-323`.)
- `winSetInstanceData` (`GameWindow.cpp:1056-1084`) does a whole-struct copy, deliberately
  *not* `memcpy`, preserving the window's own `DisplayString` pointers and nulling
  `m_videoBuffer`. It copies `m_instData.m_status` from the caller's struct — which is a
  *different* field from `GameWindow::m_status`. See the gotcha in §9.

### 3.3 Hierarchy, z-order, modality

- `linkWindow` inserts at the head of `m_windowList`, but skips past any windows that are on
  the modal stack (`GameWindowManager.cpp:253-305`) so modals stay on top.
- `winRepaint` draws the list **tail-to-head** in three passes: `WIN_STATUS_BELOW`, then
  neither-above-nor-below, then `WIN_STATUS_ABOVE` (`:1270-1304`). Head of list = drawn last =
  on top.
- `winSetModal`/`winUnsetModal` (`:1515`, `:1549`) push/pop a `ModalWindow` linked list. While
  a modal is up, `winProcessMouseEvent` only ever hit-tests inside the modal window
  (`:1003-1006`), and `winNextTab`/`winPrevTab` bail out entirely (`:4012`, `:4036`).

### 3.4 Finding controls

```c
GameWindow *w = TheWindowManager->winGetWindowFromId(
                    nullptr,                                       // search everything
                    NAMEKEY("OptionsMenu.wnd:ComboBoxResolution"));
```

`winGetWindowFromId` (`GameWindowManager.cpp:656-680`) starts at `m_windowList` if given
`nullptr`, walks `m_next`, and recurses into `m_child`. Note the recursion walks the *siblings*
of whatever you pass in too — passing a child window searches that child and everything after
it in its sibling chain, not just its subtree.

`NAMEKEY(...)` is `TheNameKeyGenerator->nameToKey(...)`
(`GeneralsMD/Code/GameEngine/Include/Common/NameKeyGenerator.h:139-140`), which is
**case-sensitive** (`NameKeyGenerator.cpp:156`) and **allocates on miss**. A typo returns a
fresh valid key that matches nothing; `winGetWindowFromId` then returns `nullptr` and most menu
code dereferences it without checking.

### 3.5 Input dispatch

`WindowXlat` (`GeneralsMD/.../MessageStream/WindowXlat.cpp`) converts message-stream events
into `winProcessMouseEvent` (`:242, :268, :292`) and `winProcessKey` (`:315`) calls.

- **Mouse** (`GameWindowManager.cpp:832`): priority is mouse-captor → grab window (drag) →
  modal head → `findWindowUnderMouse`. Once a target is found the message walks *up* the
  parent chain until someone returns `MSG_HANDLED` (`:870-888`).
- **Keyboard** (`:793-827`): goes only to `m_keyboardFocus`, then bubbles up parents via
  `GWM_CHAR`. There is no global accelerator table here — hotkeys are a separate subsystem
  (`GameClient/HotKey.h`).
- `GWM_MOUSE_POS` is *not* propagated by default; it is gated on a static
  `sendMousePosMessages` flag (`GameWindow.h:133-135`, used at `GameWindowManager.cpp:864`).

### 3.6 Destruction is deferred

`winDestroy` (`:1417-1479`) marks `WIN_STATUS_DESTROYED`, recursively destroys children
(`:1452-1456`), unlinks from parent/manager, removes itself from its `WindowLayout`
(`:1474-1475`), and pushes onto `m_destroyList`. The actual `GWM_DESTROY` message and
`deleteInstance` happen later, in `processDestroyList()` from
`GameWindowManager::update()` (`:80-127`, `:241-248`).

`processDestroyList` asserts `winGetUserData() == nullptr` after `GWM_DESTROY`
(`:120`) — i.e. **every gadget's `GWM_DESTROY` handler is required to free and null its own
user data**. `GadgetPushButtonSystem` does it at `GadgetPushButton.cpp:499-504`;
`GadgetListBoxSystem` does it at `GadgetListBox.cpp:1942-1985`. If you attach user data to a
plain `USER` window, you must supply a system callback that frees it, or you trip the assert.

`WIN_MAX_WINDOWS = 576` (`GameWindow.h:103`) is declared and **never referenced anywhere** —
there is no window count limit in practice.

---

## 4. Gadgets

### 4.1 The catalogue

| `.wnd` `WINDOWTYPE` | Style bit | Created by | Data struct | Helper header |
| --- | --- | --- | --- | --- |
| `PUSHBUTTON` | `GWS_PUSH_BUTTON` | `gogoGadgetPushButton` (`:1794`) | `PushButtonData` (lazy) | `GadgetPushButton.h` |
| `CHECKBOX` | `GWS_CHECK_BOX` | `gogoGadgetCheckbox` (`:1861`) | — | `GadgetCheckBox.h` |
| `RADIOBUTTON` | `GWS_RADIO_BUTTON` | `gogoGadgetRadioButton` (`:1926`) | `RadioButtonData` | `GadgetRadioButton.h` |
| `TABCONTROL` | `GWS_TAB_CONTROL` | `gogoGadgetTabControl` (`:1998`) | `TabControlData` | `GadgetTabControl.h` |
| `SCROLLLISTBOX` | `GWS_SCROLL_LISTBOX` | `gogoGadgetListBox` (`:2069`) | `ListboxData` | `GadgetListBox.h` |
| `HORZSLIDER` / `VERTSLIDER` | `GWS_HORZ_SLIDER` / `GWS_VERT_SLIDER` | `gogoGadgetSlider` (`:2214`) | `SliderData` | `GadgetSlider.h` |
| `COMBOBOX` | `GWS_COMBO_BOX` | `gogoGadgetComboBox` (`:2338`) | `ComboBoxData` | `GadgetComboBox.h` |
| `PROGRESSBAR` | `GWS_PROGRESS_BAR` | `gogoGadgetProgressBar` (`:2544`) | — | `GadgetProgressBar.h` |
| `STATICTEXT` | `GWS_STATIC_TEXT` | `gogoGadgetStaticText` (`:2602`) | `TextData` | `GadgetStaticText.h` |
| `ENTRYFIELD` | `GWS_ENTRY_FIELD` | `gogoGadgetTextEntry` (`:2671`) | `EntryData` | `GadgetTextEntry.h` |
| `USER` | `GWS_USER_WINDOW` | `winCreate` directly (`:2039`) | — | — |
| `TABPANE` | `GWS_TAB_PANE` | `winCreate` directly (`:2056`) | — | — |

All `gogoGadget*` functions follow the same skeleton, using `gogoGadgetPushButton`
(`:1794-1856`) as the canonical example:

1. Assert the style bit is present in `instData` — bail with `assert(0)` if not (`:1805-1812`).
2. `winCreate(parent, status, x, y, w, h, Gadget<X>System, instData)`.
3. `winSetInputFunc(Gadget<X>Input)`.
4. Pick the draw func: `get<X>ImageDrawFunc()` if `WIN_STATUS_IMAGE` is set in the *window's*
   status, else `get<X>DrawFunc()` (`:1835-1838`).
5. `winSetOwner(parent)` — with `nullptr` meaning "own yourself" (`GameWindow.cpp:1229-1239`).
6. Allocate + `memcpy` the type's data struct from the caller's template into `winSetUserData`
   (e.g. listbox at `:2107-2112`).
7. `assignDefaultGadgetLook(gadget, defaultFont, defaultVisual)` (`:2818`).

### 4.2 `assignDefaultGadgetLook`

`GameWindowManager.cpp:2818-3300ish`. Always sets the font (falling back to the localised
default window font, else `"Times New Roman" 14`, `:2864-2891`). Then, **only if
`assignVisual == TRUE`** (`:2894-2895`), it fills in placeholder images (`"PushButtonEnabled"`,
`"CheckBoxEnabledBoxSelected"`, …) and garish debug colours (red/green/yellow/blue) for the
gadget's style.

`.wnd`-loaded gadgets always pass `FALSE` (`createGadget` hard-codes it everywhere, e.g.
`GameWindowManagerScript.cpp:1635`) because the file supplies real draw data. Pass `TRUE` for
a runtime-created control only if the named placeholder images exist in the mapped-image set;
otherwise you get a red/green rectangle.

### 4.3 The message protocol

Gadget → owner messages are the `G*M_*` enum in `Gadget.h:133-201`. The convention is
`mData1 = (WindowMsgData)the gadget window`, `mData2 = payload`:

| Message | Sent by | `mData2` |
| --- | --- | --- |
| `GBM_SELECTED` | push/check/radio button | packed mouse coords |
| `GBM_SELECTED_RIGHT` | push button with `WIN_STATUS_RIGHT_CLICK` | packed coords (`GadgetPushButton.cpp:307`) |
| `GBM_MOUSE_ENTERING` / `_LEAVING` | buttons with `GWS_MOUSE_TRACK` | — |
| `GLM_SELECTED` | listbox | selected row index (`GadgetListBox.cpp:854-857`) |
| `GLM_DOUBLE_CLICKED` | listbox | row index |
| `GSM_SLIDER_TRACK` / `GSM_SLIDER_DONE` | sliders | position |
| `GCM_SELECTED` | combo box | index |
| `GEM_EDIT_DONE` / `GEM_UPDATE_TEXT` | text entry | — |
| `GGM_FOCUS_CHANGE` | any gadget | window id (`GadgetPushButton.cpp:514-517`) |

Two ready-made forwarders exist for windows that only need to relay:

- `PassSelectedButtonsToParentSystem` (`GameWindowManager.cpp:136-155`) — forwards only
  `GBM_SELECTED`, `GBM_SELECTED_RIGHT`, `GBM_MOUSE_ENTERING`, `GBM_MOUSE_LEAVING`,
  `GEM_EDIT_DONE` to `winGetParent()`.
- `PassMessagesToParentSystem` (`:160-176`) — forwards everything.

Both are registered in the lexicon (`FunctionLexicon.cpp:76-77`) so `.wnd` files can name
them, and both walk the **parent** chain, which is not necessarily the owner chain.

### 4.4 Helper API shapes

The `Gadget*` headers are two things mixed together:

**Real functions** that drive gadget state. Representative set:

```c
// Buttons                       GadgetPushButton.h:72-83
void GadgetButtonSetText(GameWindow*, UnicodeString);
void GadgetButtonEnableCheckLike(GameWindow*, Bool makeCheckLike, Bool initiallyChecked);
Bool GadgetCheckLikeButtonIsChecked(GameWindow*);
void GadgetButtonSetData(GameWindow*, void*);   // NOTE: overwrites winSetUserData

// Check / radio                 GadgetCheckBox.h:77-80, GadgetRadioButton.h:80-82
Bool GadgetCheckBoxIsChecked(GameWindow*);
void GadgetCheckBoxSetChecked(GameWindow*, Bool);
void GadgetRadioSetSelection(GameWindow*, Bool sendMsg);
void GadgetRadioSetGroup(GameWindow*, Int group, Int screen);

// Static text                   GadgetStaticText.h:70-72
void GadgetStaticTextSetText(GameWindow*, UnicodeString);
UnicodeString GadgetStaticTextGetText(GameWindow*);

// Text entry                    GadgetTextEntry.h:67-73
void GadgetTextEntrySetText(GameWindow*, UnicodeString);      // sends GEM_SET_TEXT
UnicodeString GadgetTextEntryGetText(GameWindow*);
void GadgetTextEntrySetMaxLen(GameWindow*, Short);

// Slider                        GadgetSlider.h:73-99
void GadgetSliderGetMinMax(GameWindow*, Int *min, Int *max);  // reads SliderData via winGetUserData
void GadgetSliderSetPosition(GameWindow*, Int);               // sends GSM_SET_SLIDER
Int  GadgetSliderGetPosition(GameWindow*);

// Listbox                       GadgetListBox.h:67-123
Int  GadgetListBoxAddEntryText(GameWindow*, UnicodeString, Color, Int row, Int col = -1, Bool overwrite = TRUE);
Int  GadgetListBoxAddEntryImage(GameWindow*, const Image*, Int row, Int col = -1, ...);
void GadgetListBoxSetSelected(GameWindow*, Int);
void GadgetListBoxGetSelected(GameWindow*, Int *selectList);
void GadgetListBoxReset(GameWindow*);
void GadgetListBoxSetItemData(GameWindow*, void *data, Int row, Int col = 0);
void*GadgetListBoxGetItemData(GameWindow*, Int row, Int col = 0);
void GadgetListboxCreateScrollbar(GameWindow*);
void GadgetListBoxSetColors(GameWindow*, /* 12 colours */);   // also recolours slider+buttons

// Combo box                     GadgetComboBox.h:62-105
Int  GadgetComboBoxAddEntry(GameWindow*, UnicodeString, Color);
void GadgetComboBoxSetSelectedPos(GameWindow*, Int, Bool dontHide = FALSE);
void GadgetComboBoxGetSelectedPos(GameWindow*, Int *out);
void GadgetComboBoxSetItemData(GameWindow*, Int index, void *data);
void GadgetComboBoxReset(GameWindow*);
```

**Inline image/colour accessors** that are just indexed writes into the instance data's nine
`WinDrawData` slots (`WinInstanceData.h:70, 144-146`). E.g.
`GadgetButtonSetEnabledImage(g,img)` is `g->winSetEnabledImage(0,img)` plus clearing slots 5
and 6 (`GadgetPushButton.h:84`). The **slot index meaning is per-gadget and undocumented
except by the header comment block** at the top of each file (see `GadgetPushButton.h:45-55`
for the button convention: 0 = background, 1 = selected). Listboxes use 0 = background,
1–4 = the four selected-row image pieces (`GadgetListBox.h:128-131`).

Sub-control accessors are also inline and just reach into the user-data struct:
`GadgetListBoxGetSlider/GetUpButton/GetDownButton` (`GadgetListBox.h:182-198`),
`GadgetComboBoxGetDropDownButton/GetListBox/GetEditBox` (`GadgetComboBox.h:164-190`),
`GadgetSliderGetThumb(g) == g->winGetChild()` (`GadgetSlider.h:81`).

---

## 5. Callback binding: `FunctionLexicon`

`FunctionLexicon` (`GeneralsMD/Code/GameEngine/Include/Common/FunctionLexicon.h:42`) is a
`SubsystemInterface` holding nine null-terminated arrays of
`{NameKeyType key; const char *name; void *func;}` (`:47-69`):

| Table | Loaded in | Contents |
| --- | --- | --- |
| `TABLE_GAME_WIN_SYSTEM` | `FunctionLexicon.cpp:544` | `*System` callbacks + the two `Pass*ToParentSystem` forwarders |
| `TABLE_GAME_WIN_INPUT` | `:545` | `*Input` callbacks |
| `TABLE_GAME_WIN_TOOLTIP` | `:546` | tooltip callbacks |
| `TABLE_GAME_WIN_DRAW` | `:543` | device-independent draws (only the two IME candidate draws) |
| `TABLE_GAME_WIN_DEVICEDRAW` | `W3DFunctionLexicon.cpp:142` | all `W3D*Draw` functions |
| `TABLE_WIN_LAYOUT_INIT` | `FunctionLexicon.cpp:548` | `*Init` layout callbacks |
| `TABLE_WIN_LAYOUT_DEVICEINIT` | `W3DFunctionLexicon.cpp:143` | just `W3DMainMenuInit` |
| `TABLE_WIN_LAYOUT_UPDATE` | `FunctionLexicon.cpp:549` | `*Update` layout callbacks |
| `TABLE_WIN_LAYOUT_SHUTDOWN` | `:550` | `*Shutdown` layout callbacks |

`loadTable` (`:389-413`) rewrites each entry's `key` field in place from its `name` at init
time — the table entries are non-const globals, mutated once. Lookup is a linear scan
(`keyToFunc`, `:418-438`).

Draw and layout-init lookups check the **device table first, then the generic one**
(`:688-704`, `:706-722`); system/input/tooltip lookups are restricted to their single table by
the default argument (`FunctionLexicon.h:99-101`).

### Adding a new callback name

1. Declare the function (or `extern` it) in `FunctionLexicon.cpp`.
2. Add `{ NAMEKEY_INVALID, "MyMenuSystem", (void*)MyMenuSystem },` to the right table.
3. Reference `"MyMenuSystem"` from the `.wnd` file.

The pattern is already followed for the GeneralsX-added Extras menu — `ExtrasMenuSystem`
(`FunctionLexicon.cpp:151`), `ExtrasMenuInput` (`:223`), `ExtrasMenuInit` (`:286`),
`ExtrasMenuUpdate` (`:330`), `ExtrasMenuShutdown` (`:372`).

**There is no `functionToName()` and there deliberately never will be.** The header explains
why (`FunctionLexicon.h:86-96`): in release builds the linker folds identical function bodies,
so two empty stubs collapse to one address and the mapping stops being 1:1. `validate()`
(`FunctionLexicon.cpp:619-682`) exists purely to warn about that at startup — but it is called
from `FunctionLexicon::init()` at `:552`, i.e. *before* `W3DFunctionLexicon::init()` loads the
two device tables (`W3DFunctionLexicon.cpp:139-143`), so device draw functions are never
validated.

---

## 6. `WindowLayout` and the `Shell` screen stack

### 6.1 `WindowLayout`

`Core/GameEngine/Include/GameClient/WindowLayout.h:50`. A `MemoryPoolObject` holding a
filename, a list of **root** windows, a hidden flag, and three callbacks
(`WindowLayoutInitFunc`/`UpdateFunc`/`ShutdownFunc`, all
`void(WindowLayout*, void *userData)` — `:42-44`).

`load()` (`WindowLayout.cpp:189-252`) calls `winCreateFromScript` and adds every entry of
`info.windows` (`:219-224`). The commented-out block at `:226-240` explains why it can't just
walk the manager list backwards from the returned window: modal windows sit at the head.

`runInit/runUpdate/runShutdown` are trivial null-checked inline dispatchers
(`WindowLayout.h:114-116`). Nothing calls them automatically — the `Shell` does.

`destroyWindows()` (`WindowLayout.cpp:168-183`) pops each root off the layout and calls
`winDestroy`. Children go with their parent via `winDestroy`'s recursion. The destructor
asserts the list is already empty (`:66-67`).

### 6.2 The push/pop protocol

The best documentation in the codebase for this is the comment block at
`GeneralsMD/Code/GameEngine/Include/GameClient/Shell.h:36-92`. Read it. The short version:

**Push** (`Shell.cpp:320-386`):

```
push(file)
  m_pendingPush = TRUE;  m_pendingPushName = file;              :354-355
  if (top() && !top()->isHidden())  top()->runShutdown(&shutdownImmediate);   :368-374
  else                              shutdownComplete(nullptr);                :379
```

`Shell::push` never loads anything itself. The old screen's shutdown callback is responsible
for eventually calling `TheShell->shutdownComplete(layout)`, and *that* is where the pending
push is consumed (`shutdownComplete`, `:773-823` → `doPush`, `:681-726`).

Most menus defer that call so the exit animation can play. `MainMenuShutdown` only sets
`isShuttingDown = TRUE` and reverses the animation (`MainMenu.cpp:723-770`); the real
`shutdownComplete` fires from `MainMenuUpdate` once
`TheShell->isAnimFinished() && TheTransitionHandler->isFinished()`
(`MainMenu.cpp:1052-1055`). `MapSelectMenu` does the same
(`MapSelectMenu.cpp:206-224`, `:229-240`).

**Pop** (`:393-426`) is symmetric: set `m_pendingPop`, run shutdown, wait for
`shutdownComplete` → `doPop` (`:731-761`), which unlinks, `destroyWindows()`,
`deleteInstance`, then `runInit()` on the new top.

**`popImmediate`** (`:435-464`) passes `immediatePop = TRUE` to the shutdown callback (menus
check this and call `shutdownComplete` synchronously) and then calls `doPop` directly.
`Shell::reset()` uses it in a loop to clear the stack (`:167-168`).

`doPush` in this tree has a GeneralsX null-guard added (`Shell.cpp:700-712`): before it, a
missing `.wnd` was caught only by a `DEBUG_ASSERTCRASH` that compiles out in release, and
`linkScreen` dereferenced the null. The comment cites `Menus/ExtrasMenu.wnd` — absent from
stock Zero Hour data — as the case that hit it.

### 6.3 The stack, and what is *not* on it

`m_screenStack[MAX_SHELL_STACK]` with `MAX_SHELL_STACK == 16` (`Shell.h:171-172`).
`Shell::update()` (`Shell.cpp:178-218`) throttles to ~30 Hz and calls `runUpdate(nullptr)` on
**every** layout on the stack, top to bottom (`:192-198`), then ticks the
`AnimateWindowManager` and `ShellMenuSchemeManager`.

Three layouts are held *outside* the stack, as long-lived hide/show overlays
(`Shell.h:196-198`, created lazily at `Shell.cpp:889-936`):

- `m_saveLoadMenuLayout` — `Menus/PopupSaveLoad.wnd`
- `m_popupReplayLayout` — `Menus/PopupReplay.wnd`
- `m_optionsLayout` — `Menus/OptionsMenu.wnd`

The comment at `Shell.h:186-195` explains the motivation: destroying a menu from inside its
own button handler runs the destroy list before control returns from the window procedure, so
hiding is safer than destroying.

**These overlays never receive `runUpdate`,** because `Shell::update` only iterates the stack.
That is why `OptionsMenuUpdate` is an empty function (`OptionsMenu.cpp:1464-1467`) and
`OptionsMenuShutdown` is entirely commented out (`:1440-1459`) — neither would ever run.

### 6.4 Animation and transitions

Two independent systems gate "is the screen done animating":

- `AnimateWindowManager` — per-window slide/fade, registered explicitly by menu init code:
  `TheShell->registerWithAnimateManager(win, WIN_ANIMATION_SLIDE_RIGHT, TRUE, 0)`
  (`MapSelectMenu.cpp:188-189`). Wrapped by `Shell::registerWithAnimateManager` /
  `isAnimFinished` / `reverseAnimatewindow` (`Shell.cpp:826-876`), all of which no-op when
  `TheGlobalData->m_animateWindows` is off.
- `TheTransitionHandler` (`GameWindowTransitions.h`) — named transition *groups* driven from
  INI, e.g. `TheTransitionHandler->setGroup("MainMenuFactionUS")`
  (`MainMenu.cpp:1246`). Owned by `GameWindowManager::init()` (`GameWindowManager.cpp:219-222`)
  and drawn at the end of `winRepaint` (`:1302-1303`).

`Shell::isAnimFinished()` requires *both* to be finished (`Shell.cpp:837-852`).

---

## 7. How a menu is actually wired

### 7.1 `MapSelectMenu` — the clean, representative case

File: `GeneralsMD/Code/GameEngine/Source/GameClient/GUI/GUICallbacks/Menus/MapSelectMenu.cpp`.

| Callback | Registered as | Does |
| --- | --- | --- |
| `MapSelectMenuInit` (`:152`) | `LAYOUTINIT` | `layout->hide(FALSE)`, look up the listbox by name, populate it, set focus to the parent window, register slide animations, restore the radio-button state |
| `MapSelectMenuUpdate` (`:229`) | `LAYOUTUPDATE` | polls `TheShell->isAnimFinished()` and then either starts the game or calls the local `shutdownComplete` |
| `MapSelectMenuShutdown` (`:206`) | `LAYOUTSHUTDOWN` | if `*(Bool*)userData` (immediate pop) finish now, else reverse the animation and let `Update` finish it |
| `MapSelectMenuSystem` (`:299`) | root window `SYSTEMCALLBACK` | caches `NameKey`s on `GWM_CREATE`, claims keyboard focus on `GWM_INPUT_FOCUS`, dispatches `GBM_SELECTED` by control id |
| `MapSelectMenuInput` (`:245`) | root window `INPUTCALLBACK` | ESC handling etc. |

The local static `shutdownComplete` helper (`:89`) is the mandatory
`layout->hide(TRUE); TheShell->shutdownComplete(layout);` pair. Every menu in the tree defines
its own copy of that function with the same name and `static` linkage — `MainMenu.cpp:330`,
`MapSelectMenu.cpp:89`, and so on. Don't confuse it with `Shell::shutdownComplete`.

Dispatch is a long `if/else if` chain on `control->winGetWindowId()`
(`MapSelectMenu.cpp:347-...`), with the ids either cached in file-statics
(`radioButtonSystemMapsID`, `:52`) or resolved inline via a function-local
`static NameKeyType x = NAMEKEY("...")` (`:355-356`).

### 7.2 `OptionsMenu` — the overlay case, and its wrinkles

File: `.../Menus/OptionsMenu.cpp`. Opened from the main menu with
(`MainMenu.cpp:1588-1592`):

```c
WindowLayout *optLayout = TheShell->getOptionsLayout(TRUE);   // creates on first use
optLayout->runInit();
optLayout->hide(FALSE);
optLayout->bringForward();
```

Closed by an explicit `TheShell->destroyOptionsLayout()` from a local helper
(`OptionsMenu.cpp:898-904`), reached from the Back and Accept buttons
(`:1647`, `:1673`).

`OptionsMenuInit` (`:947-...`) is a ~200-line block of
`x = NAMEKEY("OptionsMenu.wnd:Y"); xWin = winGetWindowFromId(nullptr, x);` caching every
control into a file-static. The file ends with a load-bearing comment,
`// MUST NEVER ADD ANOTHER OPTION HERE AT THE END !` (`:895`) — no explanation is given for
why; treat it as a landmine and add options above it.

The wrinkle worth flagging: `OptionsMenuSystem` calls `TheShell->push("Menus/KeyboardOptionsMenu.wnd")`
and `TheShell->push("Menus/ExtrasMenu.wnd")` (`:1696`, `:1700`) *while the options overlay is
open*. Because the overlay is not on the shell stack, `Shell::push` shuts down whatever is at
`top()` — the main menu — and the options windows stay on screen underneath the newly pushed
screen. If sub-screens opened from Options look wrong, this is why.

### 7.3 In-game GUI

The command bar is a separate subsystem (`GUI/ControlBar/`, `GameClient/ControlBar.h`) that
loads `ControlBar.wnd` and drives the same `GameWindow`/gadget primitives, with its own
callbacks registered in the lexicon (`ControlBarSystem`, `ControlBarInput`,
`FunctionLexicon.cpp:136-137`) and its own W3D draw functions
(`W3DFunctionLexicon.cpp:87-95`). It is out of scope here; the window mechanics are identical.

---

## 8. Adding a NEW control at runtime, without editing `.wnd` files

### 8.1 Why this matters here

`GUIEdit` — the tool that authors `.wnd` files — is a Win32 program: it links `comctl32`,
`imm32`, `vfw32`, builds a `.rc` resource file, and is declared `add_executable(z_guiedit WIN32)`
(`GeneralsMD/Code/Tools/GUIEdit/CMakeLists.txt:38, 48-67`). It is still *configured* in the
macOS build tree (`RTS_BUILD_ZEROHOUR_TOOLS` defaults `ON`,
`cmake/config-build.cmake:54`; the `ios-vulkan` preset turns it off explicitly,
`CMakePresets.json:271`) but it is not part of the `z_generals` target and cannot run on
macOS or iOS. Adding UI on this port therefore means either hand-editing `.wnd` text or
creating windows in code.

### 8.2 The recipe

There are two in-tree precedents, both GeneralsX additions in `MainMenu.cpp`. This is the
push-button one (`MainMenu.cpp:889-931`), lightly annotated:

```c
static GameWindow *s_myButton = nullptr;     // file-static handle

// ... inside the layout's Update or Init callback ...
if (s_myButton == nullptr && TheDisplay && parentMainMenu)
{
    const Int w = 260, h = 26, margin = 8;
    const Int x = TheDisplay->getWidth()  - w - margin;   // you scale it yourself
    const Int y = TheDisplay->getHeight() - h - margin;

    WinInstanceData instData;
    instData.init();                                       // WinInstanceData.cpp:102
    BitSet(instData.m_style, GWS_PUSH_BUTTON | GWS_MOUSE_TRACK);  // REQUIRED, asserted
    instData.m_id = NAMEKEY("Runtime:MyButton");           // optional but recommended
    instData.m_status = WIN_STATUS_ENABLED;                // see gotcha below
    instData.setTooltipText(tooltip);

    s_myButton = TheWindowManager->gogoGadgetPushButton(
                     parentMainMenu,                       // parent AND owner
                     WIN_STATUS_ENABLED,                   // GameWindow::m_status
                     x, y, w, h,
                     &instData,
                     nullptr,                              // font: nullptr -> default
                     TRUE);                                // defaultVisual: placeholder art

    if (s_myButton)
    {
        GameFont *font = TheWindowManager->winFindFont("Arial", 10, FALSE);
        if (font) s_myButton->winSetFont(font);
        GadgetButtonSetText(s_myButton, displayText);       // do this LAST
    }
}
```

Handling the click, in the parent's existing system callback
(`MainMenu.cpp:1614-1618`):

```c
case GBM_SELECTED:
{
    GameWindow *control = (GameWindow *)mData1;
    if (s_myButton != nullptr && control == s_myButton) { ... }   // pointer compare
    else if (control->winGetWindowId() == someOtherID) { ... }
}
```

Cleanup, in the layout's shutdown (`MainMenu.cpp:736-740`):

```c
if (s_myButton) { TheWindowManager->winDestroy(s_myButton); s_myButton = nullptr; }
```

### 8.3 Checklist

- **Parent.** Pass the screen's root window (looked up by its `.wnd` name). This makes the new
  control a child, so it is destroyed with the parent, positioned relative to it, and — because
  `gogoGadget*` sets the owner to the parent — its `GBM_*`/`GLM_*` messages land in the
  existing menu system callback. Passing `nullptr` makes it a top-level window that owns
  itself, which means *no one* receives its messages unless you set an owner explicitly.
- **`instData.init()` first, always.** `WinInstanceData`'s constructor calls it, but a
  reused stack instance needs it (`WinInstanceData.h:103-106`).
- **Style bit is mandatory.** Every `gogoGadget*` asserts on it and returns `nullptr`
  otherwise (`GameWindowManager.cpp:1805-1812`, `:2084-2091`, `:2352-2359`).
- **Set both statuses.** Pass the status as the argument (that becomes `GameWindow::m_status`),
  *and* set `instData.m_status` if you need `WIN_STATUS_RIGHT_CLICK` or
  `WIN_STATUS_WRAP_CENTERED` — see §9.
- **`WIN_STATUS_IMAGE` must be in the status argument**, not set afterwards: the image vs
  non-image draw function is chosen once at creation (`:1835-1838`).
- **Give it an ID** if anything else needs to find it:
  `instData.m_id = NAMEKEY("Runtime:MyButton")` before the call, or
  `win->winSetWindowId(NAMEKEY(...))` after. With no ID the window's id is `0`
  (`WinInstanceData.cpp:132`), which is `NAMEKEY_INVALID`
  (`NameKeyGenerator.h:46`), and `winGetWindowFromId(nullptr, 0)` will return the first
  unnamed window it finds anywhere in the process.
- **Positioning is yours.** No `CREATIONRESOLUTION` scaling happens on this path; use
  `TheDisplay->getWidth()/getHeight()` (as both in-tree examples do) or scale from 800×600
  by hand.
- **Adding to a `WindowLayout`.** Only necessary if you create a *root* window and want
  `layout->hide()` / `destroyWindows()` to cover it: `layout->addWindow(win)`
  (`WindowLayout.cpp:94-125`). Children of a window already in the layout need nothing.

### 8.4 Per-type extra requirements

**Static text** (`MainMenu.cpp:443-465` is a working example): allocate a zeroed `TextData`
and pass it. `TextData` (`Gadget.h:305-314`) carries `centered`, `centeredVertically`,
`leftMargin`, `topMargin`.

**List box**: fill a `ListboxData` (`Gadget.h:343-383`) with at least `listLength`, `columns`,
`scrollBar`. The struct is `memcpy`'d, not deep-copied (`GameWindowManager.cpp:2107-2109`), so:

- If `columns > 1` you must supply `columnWidthPercentage` as a **heap `new Int[columns]`**
  array (`:2183-2187` returns `nullptr` if it's missing) — the gadget takes ownership and
  `delete[]`s it in `GWM_DESTROY` (`GadgetListBox.cpp:1975`).
- `listLength` is deliberately zeroed and re-applied through `GadgetListBoxSetListLength`
  so the row/selection arrays get allocated (`GameWindowManager.cpp:2140-2142`; the comment
  literally says `// hacky!`).

**Combo box** is the sharpest edge. `gogoGadgetComboBox` **deletes the `entryData` and
`listboxData` pointers you hand it** (`GameWindowManager.cpp:2476`, `:2494`) after using them
to build the sub-controls, then re-points them at the sub-controls' own copies. You must
allocate both with `NEW`, exactly as the `.wnd` path does
(`GameWindowManagerScript.cpp:1832-1857`). Passing stack addresses is a delete-on-non-heap
crash.

**Slider**: `SliderData` needs `minVal`/`maxVal`; `numTicks`/`position` are internal
(`Gadget.h:259-270`).

**Text entry**: `EntryData` needs `maxTextLen` and the filter flags; the `DisplayString*`
members are internal (`Gadget.h:274-301`).

### 8.5 Alternative: a whole screen without a `.wnd`

Nothing forces a screen to come from a file. You can:

```c
WindowLayout *layout = newInstance(WindowLayout);
GameWindow *root = TheWindowManager->winCreate(nullptr, WIN_STATUS_ENABLED,
                                               0, 0, w, h, MyScreenSystem);
root->winSetWindowId(NAMEKEY("MyScreen:Root"));
layout->addWindow(root);
layout->setInit(MyScreenInit);  layout->setUpdate(...); layout->setShutdown(...);
// ... create children with gogoGadget* ...
```

but such a layout cannot be driven by `Shell::push` (which always loads from a filename,
`Shell.cpp:694`). You would either manage it yourself as an overlay — the pattern
`Shell::getOptionsLayout` uses (`Shell.cpp:923-936`) — or add a Shell entry point that accepts
a pre-built layout. There is no such entry point today.

For simple confirmations, don't build anything: `MessageBoxYesNo`, `MessageBoxOk`,
`MessageBoxOkCancel` etc. (`GeneralsMD/Code/GameEngine/Include/GameClient/MessageBox.h:34-46`) sit on top
of `GameWindowManager::gogoMessageBox` (`GameWindowManager.cpp:1613-1623`) and build a modal
popup entirely in code.

---

## 9. Gotchas

Ordered roughly by how likely they are to cost you an afternoon.

**1. Tab-key navigation is 95 % dead code.** `GameWindow::winNextTab()` and
`winPrevTab()` have their entire bodies commented out and just `return WIN_ERR_OK`
(`GameWindow.cpp:370-400`, `:405-437`). The leaf-walking helpers they used
(`findFirstLeaf`/`findLastLeaf`/`findPrevLeaf`/`findNextLeaf`, `:205-360`) are consequently
unreachable, and `WIN_STATUS_TAB_STOP` is read nowhere else. There is a *second*, working
mechanism — `GameWindowManager::winNextTab(GameWindow*)` (`GameWindowManager.cpp:4010`) — but
it walks an explicitly registered `m_tabList`, and the only screen in the entire game that
registers one is `WOLLoginMenu` (`WOLLoginMenu.cpp:526-527`). Push buttons and list boxes call
the working manager version (`GadgetPushButton.cpp:439`, `GadgetListBox.cpp:685`); static text,
sliders and radio buttons call the dead per-window version (`GadgetStaticText.cpp:91`,
`GadgetHorizontalSlider.cpp:260`, `GadgetRadioButton.cpp:268`). So Tab does nothing on almost
every screen, and which gadget you're focused on decides *which* nothing happens.

**2. `WinInstanceData::m_status` and `GameWindow::m_status` are two different fields that
disagree.** `winCreate` sets `GameWindow::m_status` from its `status` argument (`:1364`) and
then copies the caller's `instData` — including `instData.m_status` — over the window's
instance data (`:1380-1381`, `GameWindow.cpp:1067`). `winGetStatus()`/`winSetStatus()` only
touch the former. Almost all code reads the former, but **`WIN_STATUS_RIGHT_CLICK`
(`GadgetPushButton.cpp:286, 332`) and `WIN_STATUS_WRAP_CENTERED`
(`W3DPushButton.cpp:97`, `GameWindowManager.cpp:2649`) are read from the latter**. The `.wnd`
path keeps them in sync because `createWindow` passes `instData->getStatus()` as the status
argument (`GameWindowManagerScript.cpp:2447`). Runtime creation does not. Set both.

**3. `Shell::push` is asynchronous, and the debug log claims something else.** The
GeneralsX `fprintf` at `Shell.cpp:356` says *"marked as pending, will load 'X' next frame"*.
That is true for animated menus, but when the stack is empty or the top layout is hidden,
`push` calls `shutdownComplete(nullptr)` synchronously (`:379`) and `doPush` runs inside the
`push` call. Code that pushes and then immediately touches `TheShell->top()` will behave
differently depending on which screen it was called from. `Shell::recreateWindowLayouts`
(`:256-264`) depends on exactly this — it calls `push` then immediately
`getScreenLayout(i)->hide(...)`, which only works because every non-top screen on the stack is
hidden by the time it is re-pushed.

**4. A truncated or malformed `.wnd` file hangs the process.** `parseWindow`'s main loop
(`GameWindowManagerScript.cpp:2389-2487`) ignores the return value of
`inFile->scanString(asciibuf)` at `:2393`; its only exits are the `END`/`CHILD` tokens and a
parse failure. At EOF, `scanString` returns `FALSE` with an empty string
(`RAMFile.cpp:465-484`), nothing matches, and `readUntilSemicolon` (`:252-294`) spins on a
`read()` that returns 0 without writing to the buffer (`RAMFile.cpp:262-284`). The result is an
infinite loop with `DEBUG_LOG` chatter in debug builds and **silent hang in release**.
`parseLayoutBlock` (`:2593-2630`) has the same shape and is even tighter — it doesn't even
call `readUntilSemicolon`. By contrast `parseChildWindows` *does* check (`:2165-2167`) and
`winCreateFromScript` *does* check (`:2772-2774`). If a menu never appears and the process
pegs one core, this is the first place to look.

**5. Two GeneralsX debug `fprintf`s fire on every window and every gadget.** `winCreate`
prints a `[GX-ISSUE144]` line per window (`GameWindowManager.cpp:1384-1407`) and
`assignDefaultGadgetLook` prints one per gadget (`:2868-2890`). Neither is behind an
`#ifdef`. `Shell::push`/`doPush` add several more (`Shell.cpp:322-324, 347, 356, 684-696,
724`). Loading one menu emits hundreds of unbuffered stderr lines; on iOS these go through
the system log. Worth gating before profiling anything UI-related.

**6. `nameToKey` never fails.** A typo in a control name or callback name produces a brand-new
`NameKey` (`NameKeyGenerator.cpp:148-162`) rather than `NAMEKEY_INVALID`, so
`winGetWindowFromId` returns `nullptr` and the lexicon lookup returns `nullptr`. Neither logs.
Most `*Init` functions then dereference the null window pointer. Lookups are also
case-sensitive (`:156`) while the file system is not.

**7. Window IDs are global, not per-layout.** The ID is the hash of the whole decorated
`"File.wnd:Control"` string, so uniqueness relies on the filename prefix.
`winGetWindowFromId(nullptr, id)` searches every window in the process and returns the first
match — including a hidden screen still on the stack, or the second instance of a layout
loaded twice.

**8. `gogoGadgetComboBox` deletes pointers you own.** `comboBoxDataTemplate->entryData` and
`->listboxData` are `delete`d at `GameWindowManager.cpp:2476` and `:2494`. Heap-allocate both.
Also note `:2486` does `BitSet(winInstData.m_style, WIN_STATUS_HIDDEN)` — a *status* constant
written into the *style* field. `WIN_STATUS_HIDDEN` and `GWS_HORZ_SLIDER` are both `0x10`, so
the combo box's internal list box is silently marked as a horizontal slider in its style bits.
Currently harmless (the next line does the real `winHide(TRUE)`), but it will bite anyone who
starts branching on the list box's style.

**9. `TEXT = "..."` in a `.wnd` is a CSF label, not display text.** Missing labels render as
`MISSING: 'YourText'` (`GameText.cpp:1399-1408`). See §2.5 for the push-button inconsistency.

**10. Gadget user data must be freed by the gadget's own `GWM_DESTROY`.**
`processDestroyList` asserts `winGetUserData() == nullptr` after sending the message
(`GameWindowManager.cpp:120`). If you attach data to a `USER` window or call
`GadgetButtonSetData` on a button whose system callback you replaced, you own the cleanup.

**11. `WindowLayout` and `Shell` overlays leak menu-local statics.** Every menu file keeps its
control pointers in file-statics that are only cleared in some paths (e.g.
`OptionsMenu.cpp:1640-1641` nulls two of ~40 cached pointers). After a layout is destroyed
those statics dangle. `MainMenu.cpp:736-740` is a GeneralsX fix for exactly this class of bug
on the dynamic update button; the pattern is not applied consistently elsewhere.

**12. `TheIMEManager` is `nullptr` on this port.** `CreateIMEManagerInterface()` returns
`nullptr` on non-Windows (`Core/GameEngine/Source/GameClient/GUI/IMEManager.cpp:1611-1616`);
`GameClient.cpp:358` assigns that to `TheIMEManager`. Every call site is null-checked
(`GadgetTextEntry.cpp:87, 387-396`, `Shell.cpp:163, 423, 461, 611, 717, 758`), so the
composition path is simply inert and text entry depends on SDL3 text input. Don't "fix" the
null checks.

**13. Unknown status/style flags in a `.wnd` are silently dropped.**
`parseBitString` logs and continues (`GameWindowManagerScript.cpp:241-244`). In a release
build there is no log. If a control looks unstyled, check its flag names against
`WindowStatusNames` / `WindowStyleNames` (`:140-155`) before anything else.

**14. Parent stack depth is 10, hard.** `WIN_STACK_DEPTH` (`:86`); `pushWindow` refuses beyond
that (`:387-404`). Nesting deeper than ten levels of `CHILD` silently truncates the layout.

---

## 10. Things I could not pin down

- **`// MUST NEVER ADD ANOTHER OPTION HERE AT THE END !`** (`OptionsMenu.cpp:895`, end of
  `saveOptions`). No accompanying explanation, no obvious mechanism in the surrounding code.
  My guess is an ordering dependency with the resolution-change path
  (`dispChanged` → `DoResolutionDialog`, `:1674-1677`) or the `pref->write()` in the caller,
  but I could not prove it. Treat as authoritative and add above the line.

- **Whether the `parseWindow` EOF hang is reachable in practice.** I traced the control flow
  and the `RAMFile` semantics, but I did not build a truncated `.wnd` and run it. The analysis
  is in gotcha #4; if you need certainty, the cheap experiment is to truncate a copy of
  `GeneralsZH/Data/Window/Menus/ExtrasMenu.wnd` mid-block and push it.

- **`GameWindowTransitions` / `ShellMenuScheme`.** I read the interfaces
  (`Core/GameEngine/Include/GameClient/GameWindowTransitions.h`,
  `GeneralsMD/.../ShellMenuScheme.h`) and the call sites in `MainMenu.cpp`, but not the ~15
  transition classes or the INI loading. If you need to add a transition, start at
  `GameWindowTransitionsStyles.cpp` and `Data/INI/WindowTransitions.ini`.

- **Whether `z_guiedit` actually configures-and-fails or configures-and-is-never-built on
  macOS.** `build/macos-vulkan/build.ninja` contains `z_guiedit` targets, so CMake configures
  it; the game is built with `--target z_generals` so it is never linked. I did not attempt
  `ninja z_guiedit`. Either way it is a Win32 program and cannot run here.

- **`ControlBar` internals.** Deliberately out of scope; it is a large subsystem
  (`GUI/ControlBar/`, plus `W3DControlBar.cpp`) that reuses these primitives but has its own
  command-button/INI model.
