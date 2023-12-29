--
-- Table structure for table `lfg_dungeon_group`
--
DROP TABLE IF EXISTS `lfg_dungeon_group`;
CREATE TABLE `lfg_dungeon_group` (
  `Id` int(10) unsigned NOT NULL DEFAULT '0',
  `LfgDungeonsID` int(10) unsigned NOT NULL DEFAULT '0',
  `RandomLfgDungeonsID` int(10) unsigned NOT NULL DEFAULT '0',
  `Reserved` int(10) unsigned NOT NULL DEFAULT '0',
  `GroupId` int(10) unsigned NOT NULL DEFAULT '0',
  PRIMARY KEY (`ID`)
) ENGINE=MyISAM DEFAULT CHARSET=utf8;

--
-- Table structure for table `scene_script`
--
DROP TABLE IF EXISTS `scene_script`;
CREATE TABLE `scene_script` (
  `Id` int(10) unsigned NOT NULL DEFAULT '0',
  `Name` text,
  `Script` longtext,
  `Reserved` int(10) unsigned NOT NULL DEFAULT '0',
  `SceneKey` int(10) unsigned NOT NULL DEFAULT '0',
  PRIMARY KEY (`ID`)
) ENGINE=MyISAM DEFAULT CHARSET=utf8;