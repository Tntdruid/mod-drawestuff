-- DrawEstuff guide marker: a generic, unselectable gameobject shown as a beam of light.
-- displayId 3272 is the light beam of "Uther Shrine Lightbeam"; change it here for another look.
-- 1990002 is the slim marker on quest targets (creatures to kill, objects to loot or use).
DELETE FROM `gameobject_template` WHERE `entry` IN (1990001, 1990002);
INSERT INTO `gameobject_template` (`entry`, `type`, `displayId`, `name`, `size`, `Data3`, `ScriptName`, `VerifiedBuild`) VALUES
(1990001, 5, 3272, 'Quest Guide Marker', 1, 1, 'DrawEstuffMarkerAI', 0),
(1990002, 5, 3272, 'Quest Target Marker', 0.35, 0, 'DrawEstuffMarkerAI', 0);

-- flags 16 = GO_FLAG_NOT_SELECTABLE
DELETE FROM `gameobject_template_addon` WHERE `entry` IN (1990001, 1990002);
INSERT INTO `gameobject_template_addon` (`entry`, `faction`, `flags`) VALUES
(1990001, 0, 16),
(1990002, 0, 16);
