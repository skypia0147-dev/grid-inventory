#pragma once

namespace FUI::Lang
{
    // i18n. Persisted as "!lang" = a language id ("en" / "ko" / a pack name).
    //
    // ★GI74: ENGLISH IS THE ONLY COMPILED LANGUAGE. Korean, Chinese, Japanese
    // and everything after them live as text in GridInventory_lang\, on the
    // exact same footing as a user's own translation — so a typo in the Korean
    // is a text edit, not a rebuild, and deleting the folder leaves a working
    // English UI rather than a broken one.
    //
    // English stays compiled because it is the FALLBACK: any key a pack has
    // not translated resolves here, so there has to be a copy no file can
    // delete. en.ini may still overlay it like any other language.
    //
    // X-macro: ONE row per string — the enum and the English table are both
    // generated from THIS list, so the former positional desync (a missing
    // entry nullptr-crashed at runtime with no compile error) is physically
    // impossible. Adding a string = adding one row here; translations catch up
    // through their .ini files and show English until they do.
    //   X(name, en)
    // ★★EQUIP SLOT strings. The nine slots the doll knows, plus "Accessory"
    // for every biped slot it does not -- which is what SlotAccepts already
    // files them as, so the tooltip and the doll cannot disagree.
    // ★Bethesda names the mod slots too (kModBack and friends) and they are
    // deliberately NOT used: modders ignore that intent (47 carries backpacks,
    // capes, quivers and wings alike), while the NUMBER is never wrong and is
    // what every mod page and conflict guide quotes.
    // ★Nothing multi-line may sit INSIDE the macro below -- each line needs a
    // trailing backslash and one missing ends it. That is why this is here.
    #define FUI_LANG_STRINGS(X)                                                                                 \
        X(Inventory, "Inventory")   /* main title — sentence case, like every bag title */                                                        \
        X(Edit, "EDIT")                                                                                     \
        X(Settings, "SETTINGS")                                                                             \
        X(ScaleLabel, "SCALE")                                                                        \
        X(FontScaleLabel, "TEXT SIZE")   /* player's font multiplier over the automatic scale */      \
        X(SkinLabel, "SKIN")                                                                                \
        X(LanguageLabel, "LANGUAGE")                                                                        \
        X(Gold, "GOLD")                                                                                     \
        X(Items, "ITEMS")                                                                                   \
        X(EquipTab, "EQUIP")                                                                                \
        X(CloseHint, "I / ESC to close")                                                                    \
        X(ResetDefault, "Reset to default")                                                                 \
        X(SaveCategory, "Save as category default")                                                         \
        X(Preset, "PRESET")                                                                                 \
        X(Save, "Save")                                                                                     \
        X(Load, "Load")                                                                                     \
        X(LoadHint, "Load replaces your current setup (items + category defaults)")                         \
        X(Bag, "Bag")                                                                                       \
        X(FootprintHint, "Footprint - drag to paint")                                                       \
        /* EDIT: which slider block the orientation section shows */                                        \
        X(FootMove, "Move")                                                                                 \
        X(FootRotate, "Rotate")                                                                             \
        /* shown in a slider's note column while its value is being typed.       */                         \
        /* ★One word: the note column is narrow and sits under the next row's    */                         \
        /* label, so the long form ran into the item art beside it. Esc is the   */                         \
        /* universal cancel and does not need saying.                            */                         \
        X(GaugeTyping, "Enter")                                                                             \
        X(Caching, "caching icons...")                                                                      \
        X(SelectHint, "click an item to edit")                                                              \
        X(Damage, "Damage")   /* tooltip stats (I1) */                                                      \
        X(Armor, "Armor")                                                                                   \
        X(ArmorLight, "Light Armor")   /* tooltip armour class (feedback ⑪) */                              \
        X(ArmorHeavy, "Heavy Armor")                                                                        \
        X(ArmorClothing, "Clothing")                                                                        \
        X(Weight, "Weight")                                                                                 \
        X(Value, "Value")                                                                                   \
        X(SlotHead, "Head")                                                                                 \
        X(SlotBody, "Body")                                                                                 \
        X(SlotHands, "Hands")                                                                               \
        X(SlotFeet, "Feet")                                                                                 \
        X(SlotShield, "Shield")                                                                             \
        X(SlotAmulet, "Amulet")                                                                             \
        X(SlotRing, "Ring")                                                                                 \
        X(SlotCirclet, "Circlet")                                                                           \
        X(SlotEars, "Ears")                                                                                 \
        X(SlotAccessory, "Accessory")                                                                       \
        X(WeapDagger, "Dagger")                                                                             \
        X(WeapSword, "Sword")                                                                               \
        X(WeapWarAxe, "War Axe")                                                                            \
        X(WeapMace, "Mace")                                                                                 \
        X(WeapGreatsword, "Greatsword")                                                                     \
        X(WeapBattleaxe, "Battleaxe")                                                                       \
        X(WeapWarhammer, "Warhammer")                                                                       \
        X(WeapBow, "Bow")                                                                                   \
        X(WeapCrossbow, "Crossbow")                                                                         \
        X(WeapStaff, "Staff")                                                                               \
        X(InventoryFull, "Inventory is full - no room for this item")   /* capacity: pickup blocked */      \
        X(BuyPresetTab, "Buy Preset Tab")   /* L2 popups */                                                 \
        X(CostLabel, "Cost")                                                                                \
        X(NotEnoughGold, "Not enough gold")                                                                 \
        X(Confirm, "OK")                                                                                    \
        X(MaxLabel, "MAX")   /* GI46 */                                                                     \
        X(Cancel, "Cancel")                                                                                 \
        X(DeleteLabel, "Delete")                                                                            \
        X(DeletePresetConfirm, "Delete this preset?")                                                       \
        X(Overloaded, "Inventory space exceeded - movement slowed")   /* W2 encumbrance */                  \
        X(StatArmor, "ARMOR")   /* S1 stats panel */                                                        \
        X(StatDamage, "WEAPON DMG")                                                                         \
        X(StatSpeed, "ATK SPEED")                                                                           \
        X(StatCrit, "CRITICAL")                                                                             \
        X(StatSpace, "SPACE")                                                                               \
        X(Withdraw, "Withdraw")   /* G2 coin pouch */                                                       \
        X(StoredLabel, "Stored")                                                                            \
        X(RechargeNoGems, "No filled soul gems")   /* (1.3.1) hover+T recharge */                           \
        X(RechargeFull, "Already fully charged")                                                            \
        X(Teaches, "Teaches")   /* tooltip extras (I1+) */                                                  \
        X(Known, "known")                                                                                   \
        X(PoisonLabel, "Poison")                                                                            \
        X(ChargeLabel, "Charge")                                                                            \
        X(TemperLabel, "Tempered")                                                                          \
        X(SoulLabel, "Soul")                                                                                \
        X(SoulPetty, "Petty")                                                                               \
        X(SoulLesser, "Lesser")                                                                             \
        X(SoulCommon, "Common")                                                                             \
        X(SoulGreater, "Greater")                                                                           \
        X(SoulGrand, "Grand")                                                                               \
        X(IconStyleLabel, "ICON STYLE")   /* two-pak icon style */                                          \
        X(StyleRealistic, "Realistic")                                                                      \
        X(StyleStylized, "Stylized")                                                                        \
        X(StyleFlat, "Drawn")   /* GI52 category icons */                                                   \
        X(StylePixel, "Pixel")  /* derived from the realistic capture */                                    \
        X(GlowLabel, "GLOW")   /* settings: rarity glow */                                                  \
        X(GlowSilhouette, "Silhouette")                                                                     \
        X(GlowRadial, "Radial")                                                                             \
        X(GlowBrightLabel, "GLOW LEVEL")   /* retired with the rarity halo */                               \
        X(ShadowDistLabel, "SHADOW DIST")   /* px toward lower-right; 0 = ambient */                        \
        X(ShadowBlurLabel, "SHADOW BLUR")   /* px of spread; 0 = hard outline */                            \
        X(ShadowOpacLabel, "SHADOW OPACITY")                                                                \
        X(IconBrightLabel, "ICON LIGHT")                                                                    \
        X(CacheLabel, "ICON CACHE")                                                                         \
        X(CacheReset, "Reset")                                                                              \
        X(CaptureLightLabel, "CAPTURE LIGHT")   /* global lamp angle every icon is shot under */            \
        X(CaptureLightHint, "Moves the lamp for EVERY icon -- they re-photograph as they appear. Set this first, then precache, then tune single items in EDIT.") \
        X(BuyLabel, "Buy")   /* Phase 4 barter */                                                           \
        X(SellLabel, "Sell")                                                                                \
        X(MerchantGoldLabel, "Merchant Gold")                                                               \
        X(MerchantNoGold, "Merchant can't afford that")                                                     \
        X(SellFavoriteConfirm, "Sell this favorited item?")                                                 \
        X(MerchantWontBuy, "The merchant doesn't deal in that")   /* Phase 6 restriction */                 \
        X(QuestItemLocked, "Quest items can't be removed")   /* Phase 7 quest guard */                      \
        X(TakeLabel, "Take")   /* slider action labels */                                                   \
        X(StoreLabel, "Store")                                                                              \
        X(SplitLabel, "Split")                                                                              \
        X(DropLabel, "Drop")   /* (1.5.x) R on a stack asks how many */                                     \
        /* (1.5.x) a whole-cell take that only partly fit -- the rest stayed put */                         \
        X(TookWhatFit, "Took what fit - the rest stayed behind")                                            \
        X(EquippedLabel, "Equipped")   /* shift-compare card */                                             \
        X(HintTakeAll, "R  Take all")                                                                        \
        X(PrecacheLabel, "PRECACHE ALL")                                                                    \
        /* shown after a cache reset: it re-reads the drawn PNGs too, but a file */                         \
        /* ADDED since launch is not in the virtual Data folder to be found.     */                         \
        X(IconReloadDone, "reloaded (new files need a game restart)")                                       \
        X(HintSliderReset, "Right-click: default")   /* the number is appended in code */                   \
        X(IconKeyLabel, "icon file")   /* editor: what to name a custom PNG */                              \
        X(IconKeyHint, "Click to copy. Drop a PNG with this name into GridInventory_fallback -- top line = this item only, bottom = its whole category.") \
        X(PrecacheStart, "Start")   /* GI68: icons whose model was still loading when their turn... */      \
        X(DeferredLabel, "SLOW ICONS")                                                                      \
        X(DeferredRetry, "Retry")                                                                           \
        X(DeferredForget, "Forget")                                                                         \
        X(CopyProps, "Copy props")   /* editor prop clipboard */                                            \
        X(PasteProps, "Paste props")                                                                        \
        X(SectionGeneral, "GENERAL")   /* F5 settings sections */                                           \
        X(SectionDisplay, "DISPLAY")                                                                        \
        X(SectionTrade, "TRADE")                                                                            \
        X(SectionIcons, "ICONS")                                                                            \
        X(MerchGoldSetLabel, "MERCHANT GOLD")   /* F3 */                                                    \
        X(MerchStockSetLabel, "MERCHANT BUYS")   /* F4 */                                                   \
        X(ToggleDefault, "Default")                                                                         \
        X(ToggleUnlimited, "Unlimited")                                                                     \
        X(ToggleAnything, "Anything")                                                                       \
        X(ToggleOn, "On")                                                                                   \
        X(ToggleOff, "Off")                                                                                 \
        /* settings: turn the quick wheel off and hand the key back to the game */                          \
        X(WheelEnableLabel, "QUICK WHEEL")                                                                  \
        X(StealTitle, "STEAL")   /* F6a steal container */                                                  \
        X(TrashTitle, "TRASH")   /* F2 trash window */                                                      \
        X(BagCollect, "COLLECT")   /* typed bag: pull matching items in */                                  \
        X(BagCollectTip, "Pull every %s from the pack into this bag")                                       \
        X(BagOnly, "Holds %s only")   /* typed bag tooltip */                                               \
        X(CostumeHint, "Wear this set as an appearance (stats stay with your equipment)")                   \
        X(CostumeWornHint, "You are already wearing this set")                                              \
        X(FilterAlchemy, "alchemy ingredients")                                                             \
        X(FilterOre, "ore and ingots")                                                                      \
        X(FilterHide, "hides and animal parts")                                                             \
        X(FilterPotion, "potions and poisons")                                                              \
        X(FilterSoulGem, "soul gems")                                                                       \
        X(FilterKey, "keys and lockpicks")                                                                  \
        X(TrashFavConfirm, "Trash this favorited item?")                                                    \
        X(TrashGoldBlocked, "Gold can't be trashed - drop it outside instead")                              \
        X(TrashBagBlocked, "Empty the bag before trashing it")                                              \
        X(TrashWornBlocked, "Take it off first")   /* P2/3-1: a worn bag has a tile now */                 \
        X(PickpocketTitle, "PICKPOCKET")   /* F6b */                                                        \
        X(PresetLabel, "PRESET")   /* GI46/47 */                                                            \
        X(PresetExport, "EXPORT")                                                                           \
        X(PresetImport, "Import")                                                                           \
        X(PresetMissing, "No preset file found")                                                            \
        X(PickpocketBlocked, "Stealing worn gear needs the Perfect Touch perk")                             \
        X(AmbiguousUnit, "Can't tell it apart from the worn copy")   /* GI50 tooltip control hints — the right-click verb is cont... */\
        X(ActPickUp, "pick up")                                                                             \
        X(ActEquip, "equip")                                                                                \
        X(ActRead, "read")                                                                                  \
        X(ActReread, "read again")                                                                          \
        X(BookRead, "Read")                                                                                 \
        /* ★LOTD'S OWN WORD, counted in its plugin rather than guessed: "on     \
           display" 50 times, "displayed" 46, and "donate" never once in a      \
           string a player reads. Its own messages are DBM_SetDisplayedMessage  \
           and DBM_SetDisplayedDoneMessage, and its dialogue says "the item is  \
           not on display" in as many words. It is also what we actually        \
           MEASURE -- the pedestal's reference being enabled -- so it stays     \
           true for a relic that was handed in but never put out. */            \
        X(MuseumOwed, "Unexhibited")   /* LOTD relic still owed */                                          \
        X(MuseumDone, "Exhibited")     /* already on a pedestal */                                          \
        X(ActLearn, "learn")                                                                                \
        X(ActSell, "sell")                                                                                  \
        X(ActStoreIn, "store")                                                                              \
        X(ActPlant, "plant")                                                                                \
        X(ActOpenBag, "open bag")                                                                           \
        X(ActCloseBag, "close bag")                                                                         \
        X(ActRestore, "restore")                                                                            \
        X(ActWithdraw, "withdraw")                                                                          \
        X(ActDeposit, "deposit")   /* coin tile -> pouch, that tile's amount only */                        \
        X(ActTakeIt, "take")                                                                                \
        X(ActBuy, "buy")                                                                                    \
        X(ActSteal, "steal")                                                                                \
        X(ActUnequip, "unequip")   /* GI63: potions, food and poisons are drunk/eaten, not worn... */       \
        X(ActUse, "use")                                                                                    \
        X(ActDrop, "drop")                                                                                  \
        /* ★Distinct from ActDrop: that one puts the item on the ground,                            \
           this one puts it in the open bin. Two disposals, and a player                            \
           reading one word must not have to guess which. */                                        \
        X(ActTrash, "discard")   /* RMB while the trash window is open */                           \
        X(ActFavorite, "favorite")                                                                          \
        X(ActRecharge, "recharge")   /* (1.3.1) T on an enchanted weapon that is not full */                \
        X(ActSplit, "split")                                                                                \
        X(ActCompare, "compare")                                                                            \
        X(Act3D, "3D")   /* GI62: shown under the carried item while it can be turned */                    \
        X(ActRotate, "rotate")   /* GI63 prompt bar — what THIS STATE allows, on the screen's... */         \
        X(PromptPlace, "place")                                                                             \
        X(PromptOrbit, "orbit")                                                                             \
        X(PromptZoom, "zoom")                                                                               \
        X(PromptReset, "reset")                                                                             \
        X(PromptStep, "adjust")                                                                             \
        X(PromptSelect, "select")   /* EDIT mode: click an item to edit its props */                        \
        X(SearchHint, "FIND")   /* placeholder in the ITEMS search box */                                   \
        X(EditSave, "Save")   /* EDIT: commit this item's edits to the ini */                               \
        X(EditWas, "was")   /* EDIT: "(was 90°)" — the value this session started from */                   \
        X(EditUnchanged, "unchanged")                                                                       \
        X(EditDiscarded, "changes discarded")   /* left an item without saving */                           \
        X(EditUnsaved, "unsaved")   /* marker beside the item name */                                       \
        X(HintRowRevert, "Right-click: revert")                                                             \
        X(PromptMax, "all")                                                                                 \
        X(PromptTakeAll, "take all")                                                                        \
        X(PromptQty, "set quantity")                                                                        \
        X(PromptSave, "save")                                                                               \
        X(PromptDrag, "drag")                                                                               \
        X(PromptWheel, "wheel")                                                                             \
        X(PromptClose, "close")   /* GI64 item descriptions. MISC records have no DESC field a... */        \
        X(PouchLine1, "· holding gold here clears its coin tiles")                                          \
        X(PouchLine2, "· the amount still counts as gold you carry")   /* ★%s = the right-click label from UIRoot::KeyLabel, so the... */\
        X(PouchLine3, "· %s to withdraw")                                                                   \
        X(BagLabel, "Bag")                                                                                  \
        X(BagCells, "%d x %d = %d cells")                                                                   \
        X(BagLine1, "· adds that much carrying space")                                                      \
        X(BagLine2, "· %s to open")                                                                         \
        X(PromptInspect, "inspect in 3D")   /* warnings — same bar, crimson. Each one is a thing the pla... */\
        X(WarnTrashClose, "Closing the window confirms the deletion")                                       \
        X(WarnOverload, "Out of space — you are moving slowly")                                             \
        X(WarnOverloadFix, "tidy up to clear it")                                                           \
        X(WarnPickpocket, "A failure closes the window at once")   /* GI50 tooltip restriction badges — why an action would be... */\
        X(BadgeQuest, "Quest item — can't be dropped or sold")                                              \
        X(BadgeStolen, "Stolen")                                                                            \
        X(BadgeWontBuy, "This merchant won't buy it")                                                       \
        X(InspectHint, "drag rotate · wheel zoom · R reset · C / ESC close")   /* inspect overlay */\
        X(WheelKeyLabel, "WHEEL KEY")   /* settings: rebind the quick menu's hotkey */              \
        X(WheelKeyPress, "press keys...")   /* ...and hold as many as you want */                   \
        X(WheelGroup, "group")   /* quick menu: mouse up/down moves between the two fans */          \
        X(WheelPick, "pick")                                                                        \
        X(WheelApply, "release to apply")

    enum class Str : int
    {
#define FUI_LANG_ENUM_ROW(name, en) name,
        FUI_LANG_STRINGS(FUI_LANG_ENUM_ROW)
#undef FUI_LANG_ENUM_ROW
        Count_
    };

    [[nodiscard]] int  Get();       // index into the language list
    void SetLang(int a_lang);
    [[nodiscard]] const char* T(Str a_key);

    // ---- language files ----------------------------------------------------
    // Data/SKSE/Plugins/GridInventory_lang/<id>.ini. Every language except the
    // compiled English fallback is one of these, including the four the mod
    // ships; a user's own translation is not a second-class citizen.
    //
    //   #name  = Polski                    shown in the LANGUAGE row
    //   #order = 50                        sort position in the row (default 100)
    //   #font  = C:\Windows\Fonts\...      optional; omit to keep the built-in face
    //   #range = cyrillic                  preset, or an explicit 0x0400-0x052F
    //   Inventory = EKWIPUNEK
    //
    // Keyed BY NAME, never by position, so a file written against an older
    // build keeps working when new strings are added in the middle. A key the
    // file does not carry falls back to English — there is no #base to choose
    // otherwise, because English is the only table that always exists.
    //
    // #range matters more than #font: the atlas bakes Latin + Hangul + CJK and
    // nothing else, so a Cyrillic file renders as tofu until it asks for the
    // range — even when the face on disk has the glyphs.
    //
    // A file named "en.ini" OVERLAYS the compiled English rather than adding a
    // language, which is how the shipped English is editable too.
    [[nodiscard]] const char* KeyName(Str a_key);   // "Inventory", "Edit", ...

    [[nodiscard]] int         Count();              // 1 + files found
    [[nodiscard]] const char* DisplayName(int a_index);
    [[nodiscard]] const char* Id(int a_index);      // "en" / file stem
    [[nodiscard]] int         IndexOfId(const char* a_id);   // -1 when unknown
    void                      LoadPacks();          // once, at plugin load

    // What the CURRENT language needs from the font atlas; empty when the
    // built-in atlas already covers it.
    [[nodiscard]] const char* FontPath();
    [[nodiscard]] const char* FontRange();

    // "SETTINGS" -> "Settings", for the two strings that serve as BOTH a
    // title-bar button label and a window title. A button wants caps; a title
    // wants to match the bag titles beside it, and those come from item names
    // so they can never be capitalised.
    // ★Done by transforming, not by adding a second key: a new key would be
    // missing from every language file already in the wild and those users
    // would get English. Case does not exist in CJK, so a Korean or Japanese
    // title passes through untouched, and a translator who already wrote
    // sentence case keeps exactly what they wrote.
    // ★All-caps is a DISPLAY style, not a translation — so it belongs here and
    // not in the strings. A built-in string that already reads "ARMOR" is
    // silently replaced by whatever en.ini says (rule 91: the file wins), and
    // the shipped en.ini says "Armor" — which is why the stats panel kept
    // coming out sentence case no matter what the DLL held. Applying the case
    // at the draw site makes the panel look the same with any language file,
    // including one the player edited. ASCII only, so CJK passes through.
    [[nodiscard]] inline std::string UpperCase(const char* a_s)
    {
        std::string out(a_s ? a_s : "");
        for (char& c : out) {
            const unsigned char u = static_cast<unsigned char>(c);
            if (u >= 'a' && u <= 'z') c = static_cast<char>(u - 'a' + 'A');
        }
        return out;
    }

    [[nodiscard]] inline std::string SentenceCase(const char* a_s)
    {
        std::string out(a_s ? a_s : "");
        bool seen = false;
        for (char& c : out) {
            const unsigned char u = static_cast<unsigned char>(c);
            if (u >= 'A' && u <= 'Z') {
                if (seen) c = static_cast<char>(u - 'A' + 'a');
                seen = true;
            } else if (u >= 'a' && u <= 'z') {
                seen = true;
            }
        }
        return out;
    }
}
