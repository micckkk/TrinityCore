/*
 * Copyright (C) 2008-2019 TrinityCore <https://www.trinitycore.org/>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "WorldSession.h"
#include "AzeriteItem.h"
#include "AzeritePackets.h"
#include "DB2Stores.h"
#include "Player.h"
#include "SpellHistory.h"

void WorldSession::HandleAzeriteEssenceUnlockMilestone(WorldPackets::Azerite::AzeriteEssenceUnlockMilestone& azeriteEssenceUnlockMilestone)
{
    if (!AzeriteItem::FindHeartForge(_player))
        return;

    Item* item = _player->GetItemByEntry(ITEM_ID_HEART_OF_AZEROTH);
    if (!item)
        return;

    AzeriteItem* azeriteItem = item->ToAzeriteItem();
    if (!azeriteItem || !azeriteItem->CanUseEssences())
        return;

    AzeriteItemMilestonePowerEntry const* milestonePower = sAzeriteItemMilestonePowerStore.LookupEntry(azeriteEssenceUnlockMilestone.AzeriteItemMilestonePowerID);
    if (!milestonePower || milestonePower->RequiredLevel > int32(azeriteItem->GetLevel()))
        return;

    // check that all previous milestones are unlocked
    for (AzeriteItemMilestonePowerEntry const* previousMilestone : sDB2Manager.GetAzeriteItemMilestonePowers())
    {
        if (previousMilestone == milestonePower)
            break;

        if (!azeriteItem->HasUnlockedEssenceMilestone(previousMilestone->ID))
            return;
    }

    azeriteItem->AddUnlockedEssenceMilestone(milestonePower->ID);
    _player->ApplyAzeriteItemMilestonePower(azeriteItem, milestonePower, true);
    azeriteItem->SetState(ITEM_CHANGED, _player);
}

void WorldSession::HandleAzeriteEssenceActivateEssence(WorldPackets::Azerite::AzeriteEssenceActivateEssence& azeriteEssenceActivateEssence)
{
    WorldPackets::Azerite::AzeriteEssenceSelectionResult activateEssenceResult;
    activateEssenceResult.AzeriteEssenceID = azeriteEssenceActivateEssence.AzeriteEssenceID;
    Item* item = _player->GetItemByEntry(ITEM_ID_HEART_OF_AZEROTH);
    if (!item || !item->IsEquipped())
    {
        activateEssenceResult.Reason = AzeriteEssenceActivateResult::NotEquipped;
        activateEssenceResult.Slot = azeriteEssenceActivateEssence.Slot;
        SendPacket(activateEssenceResult.Write());
        return;
    }

    AzeriteItem* azeriteItem = item->ToAzeriteItem();
    if (!azeriteItem || !azeriteItem->CanUseEssences())
    {
        activateEssenceResult.Reason = AzeriteEssenceActivateResult::ConditionFailed;
        SendPacket(activateEssenceResult.Write());
        return;
    }

    if (azeriteEssenceActivateEssence.Slot >= MAX_AZERITE_ESSENCE_SLOT || !azeriteItem->HasUnlockedEssenceSlot(azeriteEssenceActivateEssence.Slot))
    {
        activateEssenceResult.Reason = AzeriteEssenceActivateResult::SlotLocked;
        SendPacket(activateEssenceResult.Write());
        return;
    }

    UF::SelectedAzeriteEssences const* selectedEssences = azeriteItem->GetSelectedAzeriteEssences();
    // essence is already in that slot, nothing to do
    if (selectedEssences->AzeriteEssenceID[azeriteEssenceActivateEssence.Slot] == uint32(azeriteEssenceActivateEssence.AzeriteEssenceID))
        return;

    uint32 rank = azeriteItem->GetEssenceRank(azeriteEssenceActivateEssence.AzeriteEssenceID);
    if (!rank)
    {
        activateEssenceResult.Reason = AzeriteEssenceActivateResult::EssenceNotUnlocked;
        activateEssenceResult.Arg = azeriteEssenceActivateEssence.AzeriteEssenceID;
        SendPacket(activateEssenceResult.Write());
        return;
    }

    if (_player->IsInCombat())
    {
        activateEssenceResult.Reason = AzeriteEssenceActivateResult::AffectingCombat;
        activateEssenceResult.Slot = azeriteEssenceActivateEssence.Slot;
        SendPacket(activateEssenceResult.Write());
        return;
    }

    if (_player->isDead())
    {
        activateEssenceResult.Reason = AzeriteEssenceActivateResult::CantDoThatRightNow;
        activateEssenceResult.Slot = azeriteEssenceActivateEssence.Slot;
        SendPacket(activateEssenceResult.Write());
        return;
    }

    if (!_player->HasPlayerFlag(PLAYER_FLAGS_RESTING) && !_player->HasUnitFlag2(UNIT_FLAG2_ALLOW_CHANGING_TALENTS))
    {
        activateEssenceResult.Reason = AzeriteEssenceActivateResult::NotInRestArea;
        activateEssenceResult.Slot = azeriteEssenceActivateEssence.Slot;
        SendPacket(activateEssenceResult.Write());
        return;
    }

    // need to remove selected essence from another slot if selected
    int32 removeEssenceFromSlot = -1;
    for (int32 slot = 0; slot < MAX_AZERITE_ESSENCE_SLOT; ++slot)
        if (azeriteEssenceActivateEssence.Slot != uint8(slot) && selectedEssences->AzeriteEssenceID[slot] == uint32(azeriteEssenceActivateEssence.AzeriteEssenceID))
            removeEssenceFromSlot = slot;

    // check cooldown of major essence slot
    if (selectedEssences->AzeriteEssenceID[0] && (azeriteEssenceActivateEssence.Slot == 0 || removeEssenceFromSlot == 0))
    {
        for (uint32 essenceRank = 1; essenceRank <= rank; ++essenceRank)
        {
            AzeriteEssencePowerEntry const* azeriteEssencePower = ASSERT_NOTNULL(sDB2Manager.GetAzeriteEssencePower(selectedEssences->AzeriteEssenceID[0], essenceRank));
            if (_player->GetSpellHistory()->HasCooldown(azeriteEssencePower->MajorPowerDescription))
            {
                activateEssenceResult.Reason = AzeriteEssenceActivateResult::CantRemoveEssence;
                activateEssenceResult.Arg = azeriteEssencePower->MajorPowerDescription;
                activateEssenceResult.Slot = azeriteEssenceActivateEssence.Slot;
                SendPacket(activateEssenceResult.Write());
                return;
            }
        }
    }

    if (removeEssenceFromSlot != -1)
    {
        _player->ApplyAzeriteEssence(azeriteItem, selectedEssences->AzeriteEssenceID[removeEssenceFromSlot], MAX_AZERITE_ESSENCE_RANK,
            AzeriteItemMilestoneType(sDB2Manager.GetAzeriteItemMilestonePower(removeEssenceFromSlot)->Type) == AzeriteItemMilestoneType::MajorEssence, false);
        azeriteItem->SetSelectedAzeriteEssence(removeEssenceFromSlot, 0);
    }

    if (selectedEssences->AzeriteEssenceID[azeriteEssenceActivateEssence.Slot])
    {
        _player->ApplyAzeriteEssence(azeriteItem, selectedEssences->AzeriteEssenceID[azeriteEssenceActivateEssence.Slot], MAX_AZERITE_ESSENCE_RANK,
            AzeriteItemMilestoneType(sDB2Manager.GetAzeriteItemMilestonePower(azeriteEssenceActivateEssence.Slot)->Type) == AzeriteItemMilestoneType::MajorEssence, false);
    }

    _player->ApplyAzeriteEssence(azeriteItem, azeriteEssenceActivateEssence.AzeriteEssenceID, rank,
        AzeriteItemMilestoneType(sDB2Manager.GetAzeriteItemMilestonePower(azeriteEssenceActivateEssence.Slot)->Type) == AzeriteItemMilestoneType::MajorEssence, true);

    azeriteItem->SetSelectedAzeriteEssence(azeriteEssenceActivateEssence.Slot, azeriteEssenceActivateEssence.AzeriteEssenceID);
    azeriteItem->SetState(ITEM_CHANGED, _player);
}
