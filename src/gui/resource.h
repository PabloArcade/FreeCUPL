/* SPDX-License-Identifier: GPL-3.0-or-later */
/* freecuplw - resource and command numbers.  DARKSOFT. */
#ifndef FREECUPLW_RESOURCE_H
#define FREECUPLW_RESOURCE_H

#define IDR_MAIN        1
#define IDI_APP         1
#define IDB_TOOLS16     10
#define IDB_TOOLS24     11
#define IDR_GUIDE       20
#define IDR_MSGMENU     21

#define IDD_OPTIONS     100
#define IDD_DEVICES     101
#define IDD_GOTO        102
#define IDD_ABOUT       103

#define IDC_TAB         200
#define IDC_MSGLIST     201
#define IDC_TOOLBAR     202
#define IDC_STATUS      203
#define IDC_EDIT        204

/* options dialog */
#define IDC_O_JED       300
#define IDC_O_DOC       301
#define IDC_O_FUSEPLOT  302
#define IDC_O_LST       303
#define IDC_O_ABS       304
#define IDC_O_MX        305
#define IDC_O_NAMESRC   306
#define IDC_O_MIN       307
#define IDC_O_DEVICE    308
#define IDC_O_KB        309
#define IDC_O_SECURE    310
#define IDC_O_COMPAT    311
#define IDC_O_SIMAFTER  312
#define IDC_O_SIMLST    313
#define IDC_O_SIMJED    314
#define IDC_O_SIMLATCH  315
#define IDC_O_PERFILE   316

/* device list dialog */
#define IDC_D_FILTER    400
#define IDC_D_LIST      401
#define IDC_D_COUNT     402

#define IDC_G_LINE      420
#define IDC_A_VERSION   430
#define IDC_A_TOOLS     431

#define ID_FILE_NEW     1000
#define ID_FILE_OPEN    1001
#define ID_FILE_SAVE    1002
#define ID_FILE_SAVEAS  1003
#define ID_FILE_SAVEALL 1004
#define ID_FILE_CLOSE   1005
#define ID_FILE_EXIT    1006

#define ID_EDIT_UNDO    1020
#define ID_EDIT_REDO    1021
#define ID_EDIT_CUT     1022
#define ID_EDIT_COPY    1023
#define ID_EDIT_PASTE   1024
#define ID_EDIT_DELETE  1025
#define ID_EDIT_SELALL  1026
#define ID_EDIT_FIND    1027
#define ID_EDIT_FINDNEXT 1028
#define ID_EDIT_REPLACE 1029
#define ID_EDIT_GOTO    1030
#define ID_EDIT_DEVICE  1031

#define ID_VIEW_JED     1040
#define ID_VIEW_DOC     1041
#define ID_VIEW_LST     1042
#define ID_VIEW_SO      1043
#define ID_VIEW_SI      1044
#define ID_VIEW_PLD     1045
#define ID_VIEW_COLOUR  1046
#define ID_VIEW_FONT    1047
#define ID_VIEW_TAB2    1048
#define ID_VIEW_TAB4    1049
#define ID_VIEW_TAB8    1050
#define ID_VIEW_NEXT    1051
#define ID_VIEW_PREV    1052

#define ID_RUN_COMPILE  1060
#define ID_RUN_SIMULATE 1061
#define ID_RUN_STOP     1062
#define ID_RUN_OPTIONS  1063

#define ID_HELP_GUIDE   1080
#define ID_HELP_ABOUT   1081

#define ID_MSG_COPY     1090
#define ID_MSG_COPYALL  1091
#define ID_MSG_CLEAR    1092

#define ID_RECENT_FIRST 1100     /* .. +7 */
#define ID_TAB_FIRST    1200     /* .. +8: Alt+1 to Alt+9 */

#endif
