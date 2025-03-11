#ifndef UTF8_H

int utf8_strlen(const char* str);
// returns the number of codepoints in the string (not graphemes)

int utf8_decode(const char** c0z, int* n);
// c0z/n are in/out variables: c0z points at the start of the string, and n is
// the number of bytes remaining of the string. both are updated. it returns
// the codepoint for the decoded utf8 sequence, or -1 if unsuccessful.

int utf8_convert_lowercase_codepoint_to_uppercase(int lowercase_codepoint);
// returns the uppercase codepoint for a lowercase codepoint. if codepoint
// cannot be uppercase'd then it returns the input value.


#ifdef UTF8_IMPLEMENTATION

#include <assert.h>

// NOTE: the uppercase codepoints are not quite ordered, so upper-to-lower
// probably requires a separate table.
static const int codepoint_lowercase_uppercase_pairs[] = {
//a=97/A=65, b=98/B=66, etc
97,65,98,66,99,67,100,68,101,69,102,70,103,71,104,72,105,73,106,74,107,75,
108,76,109,77,110,78,111,79,112,80,113,81,114,82,115,83,116,84,117,85,118,86,
119,87,120,88,121,89,122,90,181,924,224,192,225,193,226,194,227,195,228,196,
229,197,230,198,231,199,232,200,233,201,234,202,235,203,236,204,237,205,
238,206,239,207,240,208,241,209,242,210,243,211,244,212,245,213,246,214,
248,216,249,217,250,218,251,219,252,220,253,221,254,222,255,376,257,256,
259,258,261,260,263,262,265,264,267,266,269,268,271,270,273,272,275,274,
277,276,279,278,281,280,283,282,285,284,287,286,289,288,291,290,293,292,
295,294,297,296,299,298,301,300,303,302,305,73,307,306,309,308,311,310,
314,313,316,315,318,317,320,319,322,321,324,323,326,325,328,327,331,330,
333,332,335,334,337,336,339,338,341,340,343,342,345,344,347,346,349,348,
351,350,353,352,355,354,357,356,359,358,361,360,363,362,365,364,367,366,
369,368,371,370,373,372,375,374,378,377,380,379,382,381,383,83,384,579,
387,386,389,388,392,391,396,395,402,401,405,502,409,408,410,573,411,42972,
414,544,417,416,419,418,421,420,424,423,429,428,432,431,436,435,438,437,
441,440,445,444,447,503,453,452,454,452,456,455,457,455,459,458,460,458,
462,461,464,463,466,465,468,467,470,469,472,471,474,473,476,475,477,398,
479,478,481,480,483,482,485,484,487,486,489,488,491,490,493,492,495,494,
498,497,499,497,501,500,505,504,507,506,509,508,511,510,513,512,515,514,
517,516,519,518,521,520,523,522,525,524,527,526,529,528,531,530,533,532,
535,534,537,536,539,538,541,540,543,542,547,546,549,548,551,550,553,552,
555,554,557,556,559,558,561,560,563,562,572,571,575,11390,576,11391,578,577,
583,582,585,584,587,586,589,588,591,590,592,11375,593,11373,594,11376,595,385,
596,390,598,393,599,394,601,399,603,400,604,42923,608,403,609,42924,611,404,
612,42955,613,42893,614,42922,616,407,617,406,618,42926,619,11362,620,42925,
623,412,625,11374,626,413,629,415,637,11364,640,422,642,42949,643,425,
647,42929,648,430,649,580,650,433,651,434,652,581,658,439,669,42930,670,42928,
837,921,881,880,883,882,887,886,891,1021,892,1022,893,1023,940,902,941,904,
942,905,943,906,945,913,946,914,947,915,948,916,949,917,950,918,951,919,
952,920,953,921,954,922,955,923,956,924,957,925,958,926,959,927,960,928,
961,929,962,931,963,931,964,932,965,933,966,934,967,935,968,936,969,937,
970,938,971,939,972,908,973,910,974,911,976,914,977,920,981,934,982,928,
983,975,985,984,987,986,989,988,991,990,993,992,995,994,997,996,999,998,
1001,1000,1003,1002,1005,1004,1007,1006,1008,922,1009,929,1010,1017,1011,895,
1013,917,1016,1015,1019,1018,1072,1040,1073,1041,1074,1042,1075,1043,
1076,1044,1077,1045,1078,1046,1079,1047,1080,1048,1081,1049,1082,1050,
1083,1051,1084,1052,1085,1053,1086,1054,1087,1055,1088,1056,1089,1057,
1090,1058,1091,1059,1092,1060,1093,1061,1094,1062,1095,1063,1096,1064,
1097,1065,1098,1066,1099,1067,1100,1068,1101,1069,1102,1070,1103,1071,
1104,1024,1105,1025,1106,1026,1107,1027,1108,1028,1109,1029,1110,1030,
1111,1031,1112,1032,1113,1033,1114,1034,1115,1035,1116,1036,1117,1037,
1118,1038,1119,1039,1121,1120,1123,1122,1125,1124,1127,1126,1129,1128,
1131,1130,1133,1132,1135,1134,1137,1136,1139,1138,1141,1140,1143,1142,
1145,1144,1147,1146,1149,1148,1151,1150,1153,1152,1163,1162,1165,1164,
1167,1166,1169,1168,1171,1170,1173,1172,1175,1174,1177,1176,1179,1178,
1181,1180,1183,1182,1185,1184,1187,1186,1189,1188,1191,1190,1193,1192,
1195,1194,1197,1196,1199,1198,1201,1200,1203,1202,1205,1204,1207,1206,
1209,1208,1211,1210,1213,1212,1215,1214,1218,1217,1220,1219,1222,1221,
1224,1223,1226,1225,1228,1227,1230,1229,1231,1216,1233,1232,1235,1234,
1237,1236,1239,1238,1241,1240,1243,1242,1245,1244,1247,1246,1249,1248,
1251,1250,1253,1252,1255,1254,1257,1256,1259,1258,1261,1260,1263,1262,
1265,1264,1267,1266,1269,1268,1271,1270,1273,1272,1275,1274,1277,1276,
1279,1278,1281,1280,1283,1282,1285,1284,1287,1286,1289,1288,1291,1290,
1293,1292,1295,1294,1297,1296,1299,1298,1301,1300,1303,1302,1305,1304,
1307,1306,1309,1308,1311,1310,1313,1312,1315,1314,1317,1316,1319,1318,
1321,1320,1323,1322,1325,1324,1327,1326,1377,1329,1378,1330,1379,1331,
1380,1332,1381,1333,1382,1334,1383,1335,1384,1336,1385,1337,1386,1338,
1387,1339,1388,1340,1389,1341,1390,1342,1391,1343,1392,1344,1393,1345,
1394,1346,1395,1347,1396,1348,1397,1349,1398,1350,1399,1351,1400,1352,
1401,1353,1402,1354,1403,1355,1404,1356,1405,1357,1406,1358,1407,1359,
1408,1360,1409,1361,1410,1362,1411,1363,1412,1364,1413,1365,1414,1366,
4304,7312,4305,7313,4306,7314,4307,7315,4308,7316,4309,7317,4310,7318,
4311,7319,4312,7320,4313,7321,4314,7322,4315,7323,4316,7324,4317,7325,
4318,7326,4319,7327,4320,7328,4321,7329,4322,7330,4323,7331,4324,7332,
4325,7333,4326,7334,4327,7335,4328,7336,4329,7337,4330,7338,4331,7339,
4332,7340,4333,7341,4334,7342,4335,7343,4336,7344,4337,7345,4338,7346,
4339,7347,4340,7348,4341,7349,4342,7350,4343,7351,4344,7352,4345,7353,
4346,7354,4349,7357,4350,7358,4351,7359,5112,5104,5113,5105,5114,5106,
5115,5107,5116,5108,5117,5109,7296,1042,7297,1044,7298,1054,7299,1057,
7300,1058,7301,1058,7302,1066,7303,1122,7304,42570,7306,7305,7545,42877,
7549,11363,7566,42950,7681,7680,7683,7682,7685,7684,7687,7686,7689,7688,
7691,7690,7693,7692,7695,7694,7697,7696,7699,7698,7701,7700,7703,7702,
7705,7704,7707,7706,7709,7708,7711,7710,7713,7712,7715,7714,7717,7716,
7719,7718,7721,7720,7723,7722,7725,7724,7727,7726,7729,7728,7731,7730,
7733,7732,7735,7734,7737,7736,7739,7738,7741,7740,7743,7742,7745,7744,
7747,7746,7749,7748,7751,7750,7753,7752,7755,7754,7757,7756,7759,7758,
7761,7760,7763,7762,7765,7764,7767,7766,7769,7768,7771,7770,7773,7772,
7775,7774,7777,7776,7779,7778,7781,7780,7783,7782,7785,7784,7787,7786,
7789,7788,7791,7790,7793,7792,7795,7794,7797,7796,7799,7798,7801,7800,
7803,7802,7805,7804,7807,7806,7809,7808,7811,7810,7813,7812,7815,7814,
7817,7816,7819,7818,7821,7820,7823,7822,7825,7824,7827,7826,7829,7828,
7835,7776,7841,7840,7843,7842,7845,7844,7847,7846,7849,7848,7851,7850,
7853,7852,7855,7854,7857,7856,7859,7858,7861,7860,7863,7862,7865,7864,
7867,7866,7869,7868,7871,7870,7873,7872,7875,7874,7877,7876,7879,7878,
7881,7880,7883,7882,7885,7884,7887,7886,7889,7888,7891,7890,7893,7892,
7895,7894,7897,7896,7899,7898,7901,7900,7903,7902,7905,7904,7907,7906,
7909,7908,7911,7910,7913,7912,7915,7914,7917,7916,7919,7918,7921,7920,
7923,7922,7925,7924,7927,7926,7929,7928,7931,7930,7933,7932,7935,7934,
7936,7944,7937,7945,7938,7946,7939,7947,7940,7948,7941,7949,7942,7950,
7943,7951,7952,7960,7953,7961,7954,7962,7955,7963,7956,7964,7957,7965,
7968,7976,7969,7977,7970,7978,7971,7979,7972,7980,7973,7981,7974,7982,
7975,7983,7984,7992,7985,7993,7986,7994,7987,7995,7988,7996,7989,7997,
7990,7998,7991,7999,8000,8008,8001,8009,8002,8010,8003,8011,8004,8012,
8005,8013,8017,8025,8019,8027,8021,8029,8023,8031,8032,8040,8033,8041,
8034,8042,8035,8043,8036,8044,8037,8045,8038,8046,8039,8047,8048,8122,
8049,8123,8050,8136,8051,8137,8052,8138,8053,8139,8054,8154,8055,8155,
8056,8184,8057,8185,8058,8170,8059,8171,8060,8186,8061,8187,8064,8072,
8065,8073,8066,8074,8067,8075,8068,8076,8069,8077,8070,8078,8071,8079,
8080,8088,8081,8089,8082,8090,8083,8091,8084,8092,8085,8093,8086,8094,
8087,8095,8096,8104,8097,8105,8098,8106,8099,8107,8100,8108,8101,8109,
8102,8110,8103,8111,8112,8120,8113,8121,8115,8124,8126,921,8131,8140,
8144,8152,8145,8153,8160,8168,8161,8169,8165,8172,8179,8188,8526,8498,
8560,8544,8561,8545,8562,8546,8563,8547,8564,8548,8565,8549,8566,8550,
8567,8551,8568,8552,8569,8553,8570,8554,8571,8555,8572,8556,8573,8557,
8574,8558,8575,8559,8580,8579,9424,9398,9425,9399,9426,9400,9427,9401,
9428,9402,9429,9403,9430,9404,9431,9405,9432,9406,9433,9407,9434,9408,
9435,9409,9436,9410,9437,9411,9438,9412,9439,9413,9440,9414,9441,9415,
9442,9416,9443,9417,9444,9418,9445,9419,9446,9420,9447,9421,9448,9422,
9449,9423,11312,11264,11313,11265,11314,11266,11315,11267,11316,11268,
11317,11269,11318,11270,11319,11271,11320,11272,11321,11273,11322,11274,
11323,11275,11324,11276,11325,11277,11326,11278,11327,11279,11328,11280,
11329,11281,11330,11282,11331,11283,11332,11284,11333,11285,11334,11286,
11335,11287,11336,11288,11337,11289,11338,11290,11339,11291,11340,11292,
11341,11293,11342,11294,11343,11295,11344,11296,11345,11297,11346,11298,
11347,11299,11348,11300,11349,11301,11350,11302,11351,11303,11352,11304,
11353,11305,11354,11306,11355,11307,11356,11308,11357,11309,11358,11310,
11359,11311,11361,11360,11365,570,11366,574,11368,11367,11370,11369,
11372,11371,11379,11378,11382,11381,11393,11392,11395,11394,11397,11396,
11399,11398,11401,11400,11403,11402,11405,11404,11407,11406,11409,11408,
11411,11410,11413,11412,11415,11414,11417,11416,11419,11418,11421,11420,
11423,11422,11425,11424,11427,11426,11429,11428,11431,11430,11433,11432,
11435,11434,11437,11436,11439,11438,11441,11440,11443,11442,11445,11444,
11447,11446,11449,11448,11451,11450,11453,11452,11455,11454,11457,11456,
11459,11458,11461,11460,11463,11462,11465,11464,11467,11466,11469,11468,
11471,11470,11473,11472,11475,11474,11477,11476,11479,11478,11481,11480,
11483,11482,11485,11484,11487,11486,11489,11488,11491,11490,11500,11499,
11502,11501,11507,11506,11520,4256,11521,4257,11522,4258,11523,4259,
11524,4260,11525,4261,11526,4262,11527,4263,11528,4264,11529,4265,11530,4266,
11531,4267,11532,4268,11533,4269,11534,4270,11535,4271,11536,4272,11537,4273,
11538,4274,11539,4275,11540,4276,11541,4277,11542,4278,11543,4279,11544,4280,
11545,4281,11546,4282,11547,4283,11548,4284,11549,4285,11550,4286,11551,4287,
11552,4288,11553,4289,11554,4290,11555,4291,11556,4292,11557,4293,11559,4295,
11565,4301,42561,42560,42563,42562,42565,42564,42567,42566,42569,42568,
42571,42570,42573,42572,42575,42574,42577,42576,42579,42578,42581,42580,
42583,42582,42585,42584,42587,42586,42589,42588,42591,42590,42593,42592,
42595,42594,42597,42596,42599,42598,42601,42600,42603,42602,42605,42604,
42625,42624,42627,42626,42629,42628,42631,42630,42633,42632,42635,42634,
42637,42636,42639,42638,42641,42640,42643,42642,42645,42644,42647,42646,
42649,42648,42651,42650,42787,42786,42789,42788,42791,42790,42793,42792,
42795,42794,42797,42796,42799,42798,42803,42802,42805,42804,42807,42806,
42809,42808,42811,42810,42813,42812,42815,42814,42817,42816,42819,42818,
42821,42820,42823,42822,42825,42824,42827,42826,42829,42828,42831,42830,
42833,42832,42835,42834,42837,42836,42839,42838,42841,42840,42843,42842,
42845,42844,42847,42846,42849,42848,42851,42850,42853,42852,42855,42854,
42857,42856,42859,42858,42861,42860,42863,42862,42874,42873,42876,42875,
42879,42878,42881,42880,42883,42882,42885,42884,42887,42886,42892,42891,
42897,42896,42899,42898,42900,42948,42903,42902,42905,42904,42907,42906,
42909,42908,42911,42910,42913,42912,42915,42914,42917,42916,42919,42918,
42921,42920,42933,42932,42935,42934,42937,42936,42939,42938,42941,42940,
42943,42942,42945,42944,42947,42946,42952,42951,42954,42953,42957,42956,
42961,42960,42967,42966,42969,42968,42971,42970,42998,42997,43859,42931,
43888,5024,43889,5025,43890,5026,43891,5027,43892,5028,43893,5029,43894,5030,
43895,5031,43896,5032,43897,5033,43898,5034,43899,5035,43900,5036,43901,5037,
43902,5038,43903,5039,43904,5040,43905,5041,43906,5042,43907,5043,43908,5044,
43909,5045,43910,5046,43911,5047,43912,5048,43913,5049,43914,5050,43915,5051,
43916,5052,43917,5053,43918,5054,43919,5055,43920,5056,43921,5057,43922,5058,
43923,5059,43924,5060,43925,5061,43926,5062,43927,5063,43928,5064,43929,5065,
43930,5066,43931,5067,43932,5068,43933,5069,43934,5070,43935,5071,43936,5072,
43937,5073,43938,5074,43939,5075,43940,5076,43941,5077,43942,5078,43943,5079,
43944,5080,43945,5081,43946,5082,43947,5083,43948,5084,43949,5085,43950,5086,
43951,5087,43952,5088,43953,5089,43954,5090,43955,5091,43956,5092,43957,5093,
43958,5094,43959,5095,43960,5096,43961,5097,43962,5098,43963,5099,43964,5100,
43965,5101,43966,5102,43967,5103,65345,65313,65346,65314,65347,65315,
65348,65316,65349,65317,65350,65318,65351,65319,65352,65320,65353,65321,
65354,65322,65355,65323,65356,65324,65357,65325,65358,65326,65359,65327,
65360,65328,65361,65329,65362,65330,65363,65331,65364,65332,65365,65333,
65366,65334,65367,65335,65368,65336,65369,65337,65370,65338,66600,66560,
66601,66561,66602,66562,66603,66563,66604,66564,66605,66565,66606,66566,
66607,66567,66608,66568,66609,66569,66610,66570,66611,66571,66612,66572,
66613,66573,66614,66574,66615,66575,66616,66576,66617,66577,66618,66578,
66619,66579,66620,66580,66621,66581,66622,66582,66623,66583,66624,66584,
66625,66585,66626,66586,66627,66587,66628,66588,66629,66589,66630,66590,
66631,66591,66632,66592,66633,66593,66634,66594,66635,66595,66636,66596,
66637,66597,66638,66598,66639,66599,66776,66736,66777,66737,66778,66738,
66779,66739,66780,66740,66781,66741,66782,66742,66783,66743,66784,66744,
66785,66745,66786,66746,66787,66747,66788,66748,66789,66749,66790,66750,
66791,66751,66792,66752,66793,66753,66794,66754,66795,66755,66796,66756,
66797,66757,66798,66758,66799,66759,66800,66760,66801,66761,66802,66762,
66803,66763,66804,66764,66805,66765,66806,66766,66807,66767,66808,66768,
66809,66769,66810,66770,66811,66771,66967,66928,66968,66929,66969,66930,
66970,66931,66971,66932,66972,66933,66973,66934,66974,66935,66975,66936,
66976,66937,66977,66938,66979,66940,66980,66941,66981,66942,66982,66943,
66983,66944,66984,66945,66985,66946,66986,66947,66987,66948,66988,66949,
66989,66950,66990,66951,66991,66952,66992,66953,66993,66954,66995,66956,
66996,66957,66997,66958,66998,66959,66999,66960,67000,66961,67001,66962,
67003,66964,67004,66965,68800,68736,68801,68737,68802,68738,68803,68739,
68804,68740,68805,68741,68806,68742,68807,68743,68808,68744,68809,68745,
68810,68746,68811,68747,68812,68748,68813,68749,68814,68750,68815,68751,
68816,68752,68817,68753,68818,68754,68819,68755,68820,68756,68821,68757,
68822,68758,68823,68759,68824,68760,68825,68761,68826,68762,68827,68763,
68828,68764,68829,68765,68830,68766,68831,68767,68832,68768,68833,68769,
68834,68770,68835,68771,68836,68772,68837,68773,68838,68774,68839,68775,
68840,68776,68841,68777,68842,68778,68843,68779,68844,68780,68845,68781,
68846,68782,68847,68783,68848,68784,68849,68785,68850,68786,68976,68944,
68977,68945,68978,68946,68979,68947,68980,68948,68981,68949,68982,68950,
68983,68951,68984,68952,68985,68953,68986,68954,68987,68955,68988,68956,
68989,68957,68990,68958,68991,68959,68992,68960,68993,68961,68994,68962,
68995,68963,68996,68964,68997,68965,71872,71840,71873,71841,71874,71842,
71875,71843,71876,71844,71877,71845,71878,71846,71879,71847,71880,71848,
71881,71849,71882,71850,71883,71851,71884,71852,71885,71853,71886,71854,
71887,71855,71888,71856,71889,71857,71890,71858,71891,71859,71892,71860,
71893,71861,71894,71862,71895,71863,71896,71864,71897,71865,71898,71866,
71899,71867,71900,71868,71901,71869,71902,71870,71903,71871,93792,93760,
93793,93761,93794,93762,93795,93763,93796,93764,93797,93765,93798,93766,
93799,93767,93800,93768,93801,93769,93802,93770,93803,93771,93804,93772,
93805,93773,93806,93774,93807,93775,93808,93776,93809,93777,93810,93778,
93811,93779,93812,93780,93813,93781,93814,93782,93815,93783,93816,93784,
93817,93785,93818,93786,93819,93787,93820,93788,93821,93789,93822,93790,
93823,93791,125218,125184,125219,125185,125220,125186,125221,125187,
125222,125188,125223,125189,125224,125190,125225,125191,125226,125192,
125227,125193,125228,125194,125229,125195,125230,125196,125231,125197,
125232,125198,125233,125199,125234,125200,125235,125201,125236,125202,
125237,125203,125238,125204,125239,125205,125240,125206,125241,125207,
125242,125208,125243,125209,125244,125210,125245,125211,125246,125212,
125247,125213,125248,125214,125249,125215,125250,125216,125251,125217,

};

int utf8_strlen(const char* str)
{
	const char* p = str;
	int n=0;
	for (;;) {
		const unsigned char b = (unsigned char)*(p++);
		if (b==0) break;
		// 0xxxxxxx: 1-byte utf-8 char
		// 11xxxxxx: first byte in 2+-byte utf-8 char
		// 10xxxxxx: non-first byte in 2+-byte utf-8 char
		if ((b&0x80)==0 || (b&0xc0)==0xc0) n++;
	}
	return n;
}

int utf8_decode(const char** c0z, int* n)
{
	const unsigned char** c0 = (const unsigned char**)c0z;
	if (*n <= 0) return -1;
	unsigned char c = **c0;
	(*n)--;
	(*c0)++;
	if ((c & 0x80) == 0) return c & 0x7f;
	int mask = 192;
	int d;
	for (d = 1; d <= 3; d++) {
		int match = mask;
		mask = (mask >> 1) | 0x80;
		if ((c & mask) == match) {
			int codepoint = (c & ~mask) << (6*d);
			while (d > 0 && *n > 0) {
				c = **c0;
				if ((c & 192) != 128) return -1;
				(*c0)++;
				(*n)--;
				d--;
				codepoint += (c & 63) << (6*d);
			}
			return d == 0 ? codepoint : -1;
		}
	}
	return -1;
}

int utf8_convert_lowercase_codepoint_to_uppercase(int lowercase_codepoint)
{
	const int n2 = sizeof(codepoint_lowercase_uppercase_pairs) / sizeof(codepoint_lowercase_uppercase_pairs[0]);
	assert((n2&1) == 0);
	const int n = n2 >> 1;
	int left=0, right=n-1;
	while (left <= right) {
		const int mid = (left + right) >> 1;
		const int* p = &codepoint_lowercase_uppercase_pairs[mid<<1];
		const int k = p[0];
		if (k < lowercase_codepoint) {
			left = mid + 1;
		} else if (k > lowercase_codepoint) {
			right = mid - 1;
		} else {
			return p[1];
		}
	}
	return lowercase_codepoint; // not found
}

#endif//UTF8_IMPLEMENTATION

#ifdef UTF8_UNIT_TEST
#include <string.h>
int main(int argc, char** argv)
{
	{
		// uppercasing:
		assert(utf8_convert_lowercase_codepoint_to_uppercase('a') == 'A');
		assert(utf8_convert_lowercase_codepoint_to_uppercase('z') == 'Z');
		assert(utf8_convert_lowercase_codepoint_to_uppercase(230) == 198); // æ=>Æ
		assert(utf8_convert_lowercase_codepoint_to_uppercase(248) == 216); // ø=>Ø
		assert(utf8_convert_lowercase_codepoint_to_uppercase(229) == 197); // å=>Å
		assert(utf8_convert_lowercase_codepoint_to_uppercase(1013) == 917);
		assert(utf8_convert_lowercase_codepoint_to_uppercase(125251) == 125217);

		// no conversion:
		assert(utf8_convert_lowercase_codepoint_to_uppercase('A') == 'A');
		assert(utf8_convert_lowercase_codepoint_to_uppercase('Z') == 'Z');
		assert(utf8_convert_lowercase_codepoint_to_uppercase(198) == 198); // Æ=>Æ
		assert(utf8_convert_lowercase_codepoint_to_uppercase(-1) == -1);
		assert(utf8_convert_lowercase_codepoint_to_uppercase(0) == 0);
		assert(utf8_convert_lowercase_codepoint_to_uppercase(1<<24) == 1<<24);
	}

	const char* dk3 = "æøå";

	{
		assert(utf8_strlen("abc") == 3);
		assert(strlen(dk3) == 6);
		assert(utf8_strlen(dk3) == 3);
		assert(strlen("å") == 2);
		assert(utf8_strlen("å") == 1);
	}

	{
		const char* p = dk3;
		int n = strlen(dk3);
		assert(n == 6);

		assert(utf8_decode(&p, &n) == 230);
		assert(p == (dk3+2));
		assert(n == 4);

		assert(utf8_decode(&p, &n) == 248);
		assert(p == (dk3+4));
		assert(n == 2);

		assert(utf8_decode(&p, &n) == 229);
		assert(p == (dk3+6));
		assert(n == 0);

		assert(utf8_decode(&p, &n) == -1);
	}

	return 0;
}
#endif

#define UTF8_H
#endif
