/**
 * @file msgtable_en.c
 * Main Message Table Jumptables (English)
 */

#include "sf33rd/Source/Game/message/en/msgtable_en.h"
#include "common.h"
#include "sf33rd/Source/Game/message/en/msgextra_en.h"
#include "sf33rd/Source/Game/message/en/msgmenu_en.h"
#include "sf33rd/Source/Game/message/en/msgsysdir_en.h"
#include "sf33rd/Source/Game/message/en/pl00end_en.h"
#include "sf33rd/Source/Game/message/en/pl00tlk_en.h"
#include "sf33rd/Source/Game/message/en/pl00win_en.h"
#include "sf33rd/Source/Game/message/en/pl01end_en.h"
#include "sf33rd/Source/Game/message/en/pl01tlk_en.h"
#include "sf33rd/Source/Game/message/en/pl01win_en.h"
#include "sf33rd/Source/Game/message/en/pl02end_en.h"
#include "sf33rd/Source/Game/message/en/pl02tlk_en.h"
#include "sf33rd/Source/Game/message/en/pl02win_en.h"
#include "sf33rd/Source/Game/message/en/pl03end_en.h"
#include "sf33rd/Source/Game/message/en/pl03tlk_en.h"
#include "sf33rd/Source/Game/message/en/pl03win_en.h"
#include "sf33rd/Source/Game/message/en/pl04end_en.h"
#include "sf33rd/Source/Game/message/en/pl04tlk_en.h"
#include "sf33rd/Source/Game/message/en/pl04win_en.h"
#include "sf33rd/Source/Game/message/en/pl05end_en.h"
#include "sf33rd/Source/Game/message/en/pl05tlk_en.h"
#include "sf33rd/Source/Game/message/en/pl05win_en.h"
#include "sf33rd/Source/Game/message/en/pl06end_en.h"
#include "sf33rd/Source/Game/message/en/pl06tlk_en.h"
#include "sf33rd/Source/Game/message/en/pl06win_en.h"
#include "sf33rd/Source/Game/message/en/pl07end_en.h"
#include "sf33rd/Source/Game/message/en/pl07tlk_en.h"
#include "sf33rd/Source/Game/message/en/pl07win_en.h"
#include "sf33rd/Source/Game/message/en/pl08end_en.h"
#include "sf33rd/Source/Game/message/en/pl08tlk_en.h"
#include "sf33rd/Source/Game/message/en/pl08win_en.h"
#include "sf33rd/Source/Game/message/en/pl09end_en.h"
#include "sf33rd/Source/Game/message/en/pl09tlk_en.h"
#include "sf33rd/Source/Game/message/en/pl09win_en.h"
#include "sf33rd/Source/Game/message/en/pl10end_en.h"
#include "sf33rd/Source/Game/message/en/pl10tlk_en.h"
#include "sf33rd/Source/Game/message/en/pl10win_en.h"
#include "sf33rd/Source/Game/message/en/pl11end_en.h"
#include "sf33rd/Source/Game/message/en/pl11tlk_en.h"
#include "sf33rd/Source/Game/message/en/pl11win_en.h"
#include "sf33rd/Source/Game/message/en/pl12end_en.h"
#include "sf33rd/Source/Game/message/en/pl12tlk_en.h"
#include "sf33rd/Source/Game/message/en/pl12win_en.h"
#include "sf33rd/Source/Game/message/en/pl13end_en.h"
#include "sf33rd/Source/Game/message/en/pl13tlk_en.h"
#include "sf33rd/Source/Game/message/en/pl13win_en.h"
#include "sf33rd/Source/Game/message/en/pl14end_en.h"
#include "sf33rd/Source/Game/message/en/pl14win_en.h"
#include "sf33rd/Source/Game/message/en/pl15end_en.h"
#include "sf33rd/Source/Game/message/en/pl15tlk_en.h"
#include "sf33rd/Source/Game/message/en/pl15win_en.h"
#include "sf33rd/Source/Game/message/en/pl16end_en.h"
#include "sf33rd/Source/Game/message/en/pl16tlk_en.h"
#include "sf33rd/Source/Game/message/en/pl16win_en.h"
#include "sf33rd/Source/Game/message/en/pl17end_en.h"
#include "sf33rd/Source/Game/message/en/pl17win_en.h"
#include "sf33rd/Source/Game/message/en/pl18end_en.h"
#include "sf33rd/Source/Game/message/en/pl18tlk_en.h"
#include "sf33rd/Source/Game/message/en/pl18win_en.h"
#include "sf33rd/Source/Game/message/en/pl19end_en.h"
#include "sf33rd/Source/Game/message/en/pl19tlk_en.h"
#include "sf33rd/Source/Game/message/en/pl19win_en.h"

MessageTable* pl_mes_tbl[20] = { &pl00win_usa_tbl, &pl01win_usa_tbl, &pl02win_usa_tbl, &pl03win_usa_tbl,
                                 &pl04win_usa_tbl, &pl05win_usa_tbl, &pl06win_usa_tbl, &pl07win_usa_tbl,
                                 &pl08win_usa_tbl, &pl09win_usa_tbl, &pl10win_usa_tbl, &pl11win_usa_tbl,
                                 &pl12win_usa_tbl, &pl13win_usa_tbl, &pl14win_usa_tbl, &pl15win_usa_tbl,
                                 &pl16win_usa_tbl, &pl17win_usa_tbl, &pl18win_usa_tbl, &pl19win_usa_tbl };

MessageTable* pl_tlk_tbl[20] = { &pl00tlk_usa_tbl, &pl01tlk_usa_tbl, &pl02tlk_usa_tbl, &pl03tlk_usa_tbl,
                                 &pl04tlk_usa_tbl, &pl05tlk_usa_tbl, &pl06tlk_usa_tbl, &pl07tlk_usa_tbl,
                                 &pl08tlk_usa_tbl, &pl09tlk_usa_tbl, &pl10tlk_usa_tbl, &pl11tlk_usa_tbl,
                                 &pl12tlk_usa_tbl, &pl13tlk_usa_tbl, &pl13tlk_usa_tbl, &pl15tlk_usa_tbl,
                                 &pl16tlk_usa_tbl, &pl16tlk_usa_tbl, &pl18tlk_usa_tbl, &pl19tlk_usa_tbl };

MessageTable* pl_end_tbl[20] = { &pl00end_usa_tbl, &pl01end_usa_tbl, &pl02end_usa_tbl, &pl03end_usa_tbl,
                                 &pl04end_usa_tbl, &pl05end_usa_tbl, &pl06end_usa_tbl, &pl07end_usa_tbl,
                                 &pl08end_usa_tbl, &pl09end_usa_tbl, &pl10end_usa_tbl, &pl11end_usa_tbl,
                                 &pl12end_usa_tbl, &pl13end_usa_tbl, &pl14end_usa_tbl, &pl15end_usa_tbl,
                                 &pl16end_usa_tbl, &pl17end_usa_tbl, &pl18end_usa_tbl, &pl19end_usa_tbl };

MessageTable* msgSysDirTbl[] = { &msgSysDirTbl_usa };

MessageTable* msgExtraTbl[] = { &msgExtraTbl_usa };

MessageTable* msgMenuTbl[] = { &msgMenuTbl_usa };
