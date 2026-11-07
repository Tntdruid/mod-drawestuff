-- DrawEstuff markers: generic, unselectable gameobjects shown as glowing columns, both silent.
-- 1990001 marks the guide's target with the Night Elf flag capture glow of Warsong Gulch (display 7258).
-- 1990002 marks quest targets (creatures to kill, objects to loot or use) with the Orc one (display 7086),
-- smaller, so the two do not look alike. Both models are 14.6 yards tall at size 1.
-- The first version used display 3272 (Uther Shrine light beam), which plays a humming sound (10946).
-- Change displayId here for another look; players must clear their client's Cache folder to see a change.
DELETE FROM `gameobject_template` WHERE `entry` IN (1990001, 1990002);
INSERT INTO `gameobject_template` (`entry`, `type`, `displayId`, `name`, `size`, `Data3`, `ScriptName`, `VerifiedBuild`) VALUES
(1990001, 5, 7258, 'Quest Guide Marker', 1.2, 1, 'DrawEstuffMarkerAI', 0),
(1990002, 5, 7086, 'Quest Target Marker', 0.45, 0, 'DrawEstuffMarkerAI', 0);

-- flags 16 = GO_FLAG_NOT_SELECTABLE
DELETE FROM `gameobject_template_addon` WHERE `entry` IN (1990001, 1990002);
INSERT INTO `gameobject_template_addon` (`entry`, `faction`, `flags`) VALUES
(1990001, 0, 16),
(1990002, 0, 16);
