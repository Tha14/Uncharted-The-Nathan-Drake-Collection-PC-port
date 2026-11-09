// u2mp: the texts of the multiplayer menus in every language the game has (u2mp/docs/TEXTS.md).
//
// The game asks TextDb::Find (0xcddc00, textdb.cpp) for a text by id; it walks the loaded banks
// (common -> networking -> subtitles, +0x58 next bank) and returns NULL for an id no bank has.
// Its callers then show the first string of the bank blob, "UNTRANSLATED STRING!!!" (0xcddc60).
// Two ways the multiplayer menus hit that or showed nothing:
//  - ids no PS4 bank has: the PS3 game got them from title updates, DLC packs or its server
//    (profile stat labels, leaderboard footer, quit penalty, DLC notes, net-patch9's skins, game type
//    variants and playlists, the three DLC map titles);
//  - the PS4's own placeholder "_" (it draws as a blank: '_' is the game's hard space): the
//    languages PS4 added (19 sam-spanish, 20 brazilian, 21 greek, 22 czech, 23 turkish) have it
//    for 397 of the 412 networking texts and for the 98 multiplayer menu texts of the common bank
//    ("Multiplayer", "Party", "Find Match" ...).
// The hook answers both, in this order (the game's banks always first):
//  1. the game's text (TextDb::Find), unless it is the placeholder;
//  2. an alias: an id whose text is another id's (net-patch9 ids for PS4 texts / our playlists);
//  3. the imported bank of the language (text1/ps4/<n>.<category>, u2text.py: the PS4 bank completed
//     from the GOTY PS3 bank of that language, sam-spanish from spanish, brazilian from portuguese);
//  4. our own text (kOwn, every language; kMenu: greek, czech and turkish for every text their PS4
//     banks have only as "_": the 96 common menu texts and the 396 networking texts);
//  5. unlocks_text (a label from a GOTY skin's package name);
//  6. the imported English bank (an untranslated text: logged once per id and language; for
//     multiplayer texts only the ids English alone has, e.g. "Buy Game");
//  7. nothing: logged once per id and language ("texts: no text"), the game shows its fallback.
// Language numbers are the bank file numbers: the loaded bank's +0x28 language index, table
// 0x1450d18 (0x38 per language, +0x14 the number); 2 english ... 18 polish as on PS3, 7 is a second
// English, 19..23 PS4 only.
#include "texts.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "unlocks.h"

extern "C" int runtime_file_translate(const char *guest, char *out, size_t size);

namespace u2mp {
namespace {

constexpr uint64_t kLanguageTable = 0x1450d18, TB_LANGUAGE = 0x28, TB_NEXT = 0x58;
constexpr const char *kTextDir = "/app0/u2data/build/main/text1/ps4/";
constexpr int kEnglish = 2, kLanguages = 24;

// The matchmaking playlists only the PS3 server had (menus.cpp kPlaylistDefs names them by these).
constexpr uint32_t kTextAllCompetitive = sid("u2mp/playlist/all-competitive"),
                   kTextAllCompetitiveDesc = sid("u2mp/playlist/all-competitive/desc"),
                   kTextTheLab = sid("u2mp/playlist/the-lab"), kTextTheLabDesc = sid("u2mp/playlist/the-lab/desc"),
                   kTextEliminationDesc = sid("u2mp/playlist/elimination/desc");

enum Column { EN, FR, IT, DE, ES, DA, PT, NL, JA, KO, FI, NO, SV, ZH, RU, PL, ES419, PTBR, EL, CS, TR, kColumns };
// language number -> column
constexpr int8_t kColumnOf[kLanguages] = {EN, EN, EN, FR, IT, DE, ES, EN, DA, PT, NL, JA,
                                          KO, FI, NO, SV, ZH, RU, PL, ES419, PTBR, EL, CS, TR};

// Our texts for the ids no bank has. Columns: en fr it de es da pt nl ja ko fi no sv zh ru pl
// es-419 pt-BR el cs tr; nullptr: es-419 -> es, pt-BR -> pt, else en. "\\" is the game's line break.
struct Text {
    uint32_t id;
    const char *text[kColumns];
};
const Text kOwn[] = {
    // ---- Profile panel k23 (0x59ddb0) stat labels
    {0xf67d9ad2u, {"Competitive Wins", "Victoires en compétition", "Vittorie competitive", "Kompetitiv-Siege",
     "Victorias competitivas", "Konkurrencesejre", "Vitórias em Competitivo", "Competitieve overwinningen", "対戦の勝利数",
     "경쟁 플레이 승리", "Kilpailuvoitot", "Konkurranseseire", "Tävlingsvinster", "對戰勝場數", "Победы в соперничестве",
     "Zwycięstwa w rywalizacji", "Victorias en competitivo", "Vitórias no Competitivo", "Ανταγωνιστικές νίκες",
     "Výhry v soutěži", "Rekabetçi Galibiyetler"}},
    {0x70c81d2eu, {"Competitive Kills", "Victimes en compétition", "Uccisioni competitive",
     "Kompetitiv-Eliminierungen", "Eliminaciones competitivas", "Konkurrencedrab", "Mortes em Competitivo",
     "Competitieve kills", "対戦の撃破数", "경쟁 플레이 처치", "Kilpailutapot", "Konkurransedrap", "Tävlingsdödningar", "對戰擊殺數",
     "Убийства в соперничестве", "Zabicia w rywalizacji", "Eliminaciones en competitivo", "Abates no Competitivo",
     "Ανταγωνιστικές εξοντώσεις", "Zabití v soutěži", "Rekabetçi Leşler"}},
    {0xc0cd1b00u, {"Cash", "Argent", "Denaro", "Geld", "Dinero", "Kontanter", "Dinheiro", "Geld", "キャッシュ", "캐쉬",
     "Rahat", "Penger", "Pengar", "現金", "Деньги", "Pieniądze", "Efectivo", nullptr, "Μετρητά", "Peníze", "Para"}},
    // ---- Multiplayer mode table 0xdef390: Leaderboards (47a936dc: GOTY) and Trophies
    {0xc21027d1u, {"View your stats", "Consultez vos stats", "Guarda le tue statistiche", "Ihre Statistik ansehen",
     "Consulta tus estadísticas", "Se din statistik", "Consulta as tuas estatísticas", "Bekijk je statistieken",
     "自分の成績を確認します", "내 통계를 확인합니다", "Katso tilastosi", "Se statistikken din", "Se din statistik", "查看你的統計資料",
     "Посмотреть свою статистику", "Sprawdź swoje statystyki", nullptr, "Veja suas estatísticas",
     "Δες τα στατιστικά σου", "Zobrazit vaše statistiky", "İstatistiklerine göz at"}},
    {0xf1b9728cu, {"Trophies", "Trophées", "Trofei", "Trophäen", "Trofeos", "Trofæer", "Troféus", "Trofeeën",
     "トロフィー", "트로피", "Trophyt", "Troféer", "Troféer", "獎盃", "Призы", "Trofea", nullptr, nullptr, "Τρόπαια",
     "Trofeje", "Kupalar"}},
    {0x85ed01ccu, {"TROPHIES", "TROPHÉES", "TROFEI", "TROPHÄEN", "TROFEOS", "TROFÆER", "TROFÉUS", "TROFEEËN",
     "トロフィー", "트로피", "TROPHYT", "TROFÉER", "TROFÉER", "獎盃", "ПРИЗЫ", "TROFEA", nullptr, nullptr, "ΤΡΟΠΑΙΑ",
     "TROFEJE", "KUPALAR"}},
    // ---- Stats ticker k24 (0x5b3240) and leaderboard columns
    {0x780b62e0u, {"Time Played", "Durée de jeu", "Tempo di gioco", "Spielzeit", "Tiempo de juego", "Spilletid",
     "Tempo de jogo", "Speeltijd", "プレイ時間", "플레이 시간", "Peliaika", "Spilletid", "Speltid", "遊戲時間", "Время игры",
     "Czas gry", nullptr, "Tempo jogado", "Χρόνος παιχνιδιού", "Odehraná doba", "Oynama Süresi"}},
    {0xe663a74eu, {"Global", "Mondial", "Globale", "Global", "Global", "Global", "Global", "Wereldwijd", "全世界",
     "전 세계", "Maailma", "Globalt", "Globalt", "全球", "Мировой", "Globalne", nullptr, nullptr, "Παγκόσμια",
     "Celosvětově", "Küresel"}},
    {0xb9602dbbu, {"Wins", "Victoires", "Vittorie", "Siege", "Victorias", "Sejre", "Vitórias", "Overwinningen",
     "勝利数", "승리", "Voitot", "Seire", "Vinster", "勝場", "Победы", "Zwycięstwa", nullptr, nullptr, "Νίκες", "Výhry",
     "Galibiyetler"}},
    {0x966fa9aeu, {"Losses", "Défaites", "Sconfitte", "Niederlagen", "Derrotas", "Nederlag", "Derrotas", "Verliezen",
     "敗北数", "패배", "Tappiot", "Tap", "Förluster", "敗場", "Поражения", "Porażki", nullptr, nullptr, "Ήττες", "Prohry",
     "Mağlubiyetler"}},
    {0x4915c4b7u, {"Ratio", "Ratio", "Rapporto", "Quote", "Proporción", "Forhold", "Rácio", "Ratio", "比率", "비율",
     "Suhde", "Forhold", "Kvot", "比率", "Соотношение", "Stosunek", nullptr, "Proporção", "Αναλογία", "Poměr", "Oran"}},
    {0x7d24a9f9u, {"Rank", "Rang", "Posizione", "Rang", "Puesto", "Placering", "Posição", "Positie", "順位", "순위",
     "Sija", "Plassering", "Placering", "排名", "Место", "Miejsce", nullptr, nullptr, "Θέση", "Pořadí", "Sıra"}},
    // ---- Leaderboard help bar 0x1041560
    {0x320a6fc8u, {"Go to Top", "Haut de la liste", "Inizio elenco", "Zum Anfang", "Principio de la lista",
     "Til toppen", "Topo da lista", "Naar boven", "先頭へ", "맨 위로", "Alkuun", "Til toppen", "Till toppen", "回到頂端",
     "В начало", "Na początek", nullptr, "Topo da lista", "Στην κορυφή", "Na začátek", "Başa Dön"}},
    {0x8994a031u, {"Next", "Suivant", "Successivo", "Weiter", "Siguiente", "Næste", "Seguinte", "Volgende", "次へ",
     "다음", "Seuraava", "Neste", "Nästa", "下一頁", "Далее", "Dalej", nullptr, "Próximo", "Επόμενο", "Další", "Sonraki"}},
    {0x5ba2fdfbu, {"Previous", "Précédent", "Precedente", "Zurück", "Anterior", "Forrige", "Anterior", "Vorige",
     "前へ", "이전", "Edellinen", "Forrige", "Föregående", "上一頁", "Назад", "Wstecz", nullptr, nullptr, "Προηγούμενο",
     "Předchozí", "Önceki"}},
    {0xd82785e7u, {"Filter", "Filtrer", "Filtro", "Filter", "Filtro", "Filter", "Filtro", "Filter", "絞り込み", "필터",
     "Suodatin", "Filter", "Filter", "篩選", "Фильтр", "Filtr", nullptr, nullptr, "Φίλτρο", "Filtr", "Filtre"}},
    // ---- Party status line (cond 49), clan tag
    {0x69cbee70u, {"[A] players today", "[A] joueurs aujourd'hui", "[A] giocatori oggi", "[A] Spieler heute",
     "[A] jugadores hoy", "[A] spillere i dag", "[A] jogadores hoje", "[A] spelers vandaag", "本日のプレイヤー：[A]",
     "오늘의 플레이어: [A]명", "[A] pelaajaa tänään", "[A] spillere i dag", "[A] spelare i dag", "今日玩家：[A] 人",
     "Игроков сегодня: [A]", "Graczy dzisiaj: [A]", nullptr, nullptr, "[A] παίκτες σήμερα", "Dnešní hráči: [A]",
     "Bugün [A] oyuncu"}},
    {0xd4143b18u, {"Clan Tag", "Tag de clan", "Tag del clan", "Clan-Tag", "Etiqueta de clan", "Klan-tag",
     "Etiqueta do clã", "Clantag", "クランタグ", "클랜 태그", "Klaanitunniste", "Klanmerke", "Klantagg", "戰隊標籤", "Тег клана",
     "Tag klanu", nullptr, "Tag do clã", "Ετικέτα κλαν", "Klanová značka", "Klan Etiketi"}},
    {0x639e0fa8u, {"Enter Clan Tag...", "Saisir le tag de clan...", "Inserisci il tag del clan...",
     "Clan-Tag eingeben ...", "Introduce la etiqueta de clan...", "Indtast klan-tag ...",
     "Introduz a etiqueta do clã...", "Clantag invoeren...", "クランタグを入力...", "클랜 태그 입력...", "Syötä klaanitunniste...",
     "Skriv inn klanmerke ...", "Ange klantagg ...", "輸入戰隊標籤...", "Введите тег клана...", "Wpisz tag klanu...",
     "Ingresa la etiqueta de clan...", "Digite a tag do clã...", "Εισαγωγή ετικέτας κλαν...",
     "Zadejte klanovou značku...", "Klan etiketini gir..."}},
    // ---- Quit penalty: scoreboard countdown 0x65cc80, leave-game confirm 0x65e750, last-game notice
    {0x64370402u, {"Quit Penalty [A]...", "Pénalité d'abandon [A]...", "Penalità abbandono [A]...",
     "Abbruchstrafe [A] ...", "Penalización por abandono [A]...", "Straf for at forlade [A] ...",
     "Penalização por desistência [A]...", "Straf voor verlaten [A]...", "途中退出ペナルティ [A]...", "중도 이탈 페널티 [A]...",
     "Poistumisrangaistus [A]...", "Straff for å forlate [A] ...", "Straff för att lämna [A] ...", "中途退出懲罰 [A]...",
     "Штраф за выход [A]...", "Kara za wyjście [A]...", nullptr, "Penalidade por desistência [A]...",
     "Ποινή αποχώρησης [A]...", "Trest za opuštění [A]...", "Oyundan Çıkma Cezası [A]..."}},
    {0x9b0dd3c2u, {"If you leave now, you will get\\a [TEXT] second wait penalty\\and a [TEXT2] earnings penalty.",
     "Si vous quittez maintenant, vous aurez\\une pénalité d'attente de [TEXT] secondes\\et une pénalité de gains de [TEXT2].",
     "Se esci ora, riceverai\\una penalità di attesa di [TEXT] secondi\\e una penalità sui guadagni di [TEXT2].",
     "Wenn Sie das Spiel jetzt verlassen, erhalten Sie\\eine Wartestrafe von [TEXT] Sekunden\\und eine Gewinnstrafe von [TEXT2].",
     "Si abandonas ahora, recibirás\\una penalización de espera de [TEXT] segundos\\y una penalización de ganancias de [TEXT2].",
     "Hvis du forlader spillet nu, får du\\en ventestraf på [TEXT] sekunder\\og en fortjenestestraf på [TEXT2].",
     "Se saíres agora, vais receber\\uma penalização de espera de [TEXT] segundos\\e uma penalização de ganhos de [TEXT2].",
     "Als je nu vertrekt, krijg je\\een wachtstraf van [TEXT] seconden\\en een winststraf van [TEXT2].",
     "今退出すると、\\[TEXT]秒の待機ペナルティと\\[TEXT2]の獲得金ペナルティを受けます。", "지금 나가면\\[TEXT]초 대기 페널티와\\[TEXT2] 수입 페널티를 받습니다.",
     "Jos poistut nyt, saat\\[TEXT] sekunnin odotusrangaistuksen\\ja [TEXT2] ansiorangaistuksen.",
     "Hvis du forlater spillet nå, får du\\en ventestraff på [TEXT] sekunder\\og en inntektsstraff på [TEXT2].",
     "Om du lämnar nu får du\\ett väntestraff på [TEXT] sekunder\\och ett intäktsstraff på [TEXT2].",
     "現在離開將會受到\\[TEXT] 秒的等待懲罰\\以及 [TEXT2] 的收入懲罰。",
     "Если вы выйдете сейчас, вы получите\\штраф ожидания [TEXT] с\\и штраф к заработку [TEXT2].",
     "Jeśli wyjdziesz teraz, otrzymasz\\karę oczekiwania [TEXT] s\\i karę do zarobków [TEXT2].", nullptr,
     "Se você sair agora, receberá\\uma penalidade de espera de [TEXT] segundos\\e uma penalidade de ganhos de [TEXT2].",
     "Αν αποχωρήσεις τώρα, θα λάβεις\\ποινή αναμονής [TEXT] δευτερολέπτων\\και ποινή κερδών [TEXT2].",
     "Pokud nyní odejdete, dostanete\\trest čekání [TEXT] s\\a trest z výdělku [TEXT2].",
     "Şimdi çıkarsan\\[TEXT] saniyelik bekleme cezası\\ve [TEXT2] kazanç cezası alırsın."}},
    {0xe012d5e3u, {"If you leave now, you will get\\a [TEXT] second wait penalty.",
     "Si vous quittez maintenant, vous aurez\\une pénalité d'attente de [TEXT] secondes.",
     "Se esci ora, riceverai\\una penalità di attesa di [TEXT] secondi.",
     "Wenn Sie das Spiel jetzt verlassen, erhalten Sie\\eine Wartestrafe von [TEXT] Sekunden.",
     "Si abandonas ahora, recibirás\\una penalización de espera de [TEXT] segundos.",
     "Hvis du forlader spillet nu, får du\\en ventestraf på [TEXT] sekunder.",
     "Se saíres agora, vais receber\\uma penalização de espera de [TEXT] segundos.",
     "Als je nu vertrekt, krijg je\\een wachtstraf van [TEXT] seconden.", "今退出すると、\\[TEXT]秒の待機ペナルティを受けます。",
     "지금 나가면\\[TEXT]초 대기 페널티를 받습니다.", "Jos poistut nyt, saat\\[TEXT] sekunnin odotusrangaistuksen.",
     "Hvis du forlater spillet nå, får du\\en ventestraff på [TEXT] sekunder.",
     "Om du lämnar nu får du\\ett väntestraff på [TEXT] sekunder.", "現在離開將會受到\\[TEXT] 秒的等待懲罰。",
     "Если вы выйдете сейчас, вы получите\\штраф ожидания [TEXT] с.",
     "Jeśli wyjdziesz teraz, otrzymasz\\karę oczekiwania [TEXT] s.", nullptr,
     "Se você sair agora, receberá\\uma penalidade de espera de [TEXT] segundos.",
     "Αν αποχωρήσεις τώρα, θα λάβεις\\ποινή αναμονής [TEXT] δευτερολέπτων.",
     "Pokud nyní odejdete, dostanete\\trest čekání [TEXT] s.",
     "Şimdi çıkarsan\\[TEXT] saniyelik bekleme cezası alırsın."}},
    {0xbde6ab5bu, {
     "You didn't finish your last game:\\you get a [TEXT] earnings penalty\\and your skill rating drops.",
     "Vous n'avez pas terminé votre dernière partie :\\vous recevez une pénalité de gains de [TEXT]\\et votre niveau de compétence baisse.",
     "Non hai terminato l'ultima partita:\\ricevi una penalità sui guadagni di [TEXT]\\e il tuo livello di abilità diminuisce.",
     "Sie haben Ihr letztes Spiel nicht beendet:\\Sie erhalten eine Gewinnstrafe von [TEXT]\\und Ihre Fähigkeitswertung sinkt.",
     "No terminaste tu última partida:\\recibes una penalización de ganancias de [TEXT]\\y baja tu nivel de habilidad.",
     "Du gjorde ikke dit sidste spil færdigt:\\du får en fortjenestestraf på [TEXT]\\og din færdighedsvurdering falder.",
     "Não terminaste o teu último jogo:\\recebes uma penalização de ganhos de [TEXT]\\e o teu nível de perícia desce.",
     "Je hebt je laatste spel niet afgemaakt:\\je krijgt een winststraf van [TEXT]\\en je vaardigheidsscore daalt.",
     "前回のゲームを最後までプレイしなかったため、\\[TEXT]の獲得金ペナルティを受け、\\スキルレーティングが下がります。",
     "지난 게임을 끝까지 마치지 않아\\[TEXT] 수입 페널티를 받고\\실력 등급이 낮아집니다.",
     "Et pelannut edellistä peliäsi loppuun:\\saat [TEXT] ansiorangaistuksen\\ja taitoluokituksesi laskee.",
     "Du fullførte ikke det forrige spillet ditt:\\du får en inntektsstraff på [TEXT]\\og ferdighetsvurderingen din synker.",
     "Du slutförde inte din senaste match:\\du får ett intäktsstraff på [TEXT]\\och din skicklighetsrankning sjunker.",
     "你沒有完成上一場遊戲，\\因此受到 [TEXT] 的收入懲罰，\\技術評等也會降低。",
     "Вы не завершили последнюю игру:\\вы получаете штраф к заработку [TEXT],\\а ваш рейтинг мастерства снижается.",
     "Nie ukończono ostatniej gry:\\otrzymujesz karę do zarobków [TEXT],\\a twoja ocena umiejętności spada.",
     nullptr,
     "Você não terminou sua última partida:\\recebe uma penalidade de ganhos de [TEXT]\\e seu nível de habilidade cai.",
     "Δεν ολοκλήρωσες το τελευταίο σου παιχνίδι:\\λαμβάνεις ποινή κερδών [TEXT]\\και η βαθμολογία δεξιότητάς σου μειώνεται.",
     "Nedokončili jste poslední hru:\\dostáváte trest z výdělku [TEXT]\\a vaše hodnocení dovednosti klesá.",
     "Son oyununu bitirmedin:\\[TEXT] kazanç cezası alıyorsun\\ve beceri puanın düşüyor."}},
    // ---- DLC notes and messages (store rows 0x5ab0xx, DLC map pack 0x5ed2xx)
    {0x9811415du, {"DLC", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
     nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr}},
    {0xb7e89ee7u, {"Requires the Siege Expansion Pack", "Nécessite le pack d'extension Siege",
     "Richiede il pacchetto di espansione Siege", "Erfordert das Siege-Erweiterungspaket",
     "Requiere el paquete de expansión Siege", "Kræver udvidelsespakken Siege", "Requer o pacote de expansão Siege",
     "Vereist het uitbreidingspakket Siege", "「Siege」拡張パックが必要です", "Siege 확장 팩이 필요합니다",
     "Vaatii Siege-laajennuspaketin", "Krever utvidelsespakken Siege", "Kräver expansionspaketet Siege",
     "需要「Siege」擴充包", "Требуется набор дополнений Siege", "Wymaga dodatku Siege", nullptr, nullptr,
     "Απαιτεί το πακέτο επέκτασης Siege", "Vyžaduje rozšíření Siege", "Siege Genişleme Paketi gerekir"}},
    {0x6cff3670u, {"Requires the Drake's Fortune Multiplayer Pack", "Nécessite le pack multijoueur Drake's Fortune",
     "Richiede il pacchetto multigiocatore Drake's Fortune", "Erfordert das Drake's Fortune-Mehrspielerpaket",
     "Requiere el paquete multijugador Drake's Fortune", "Kræver multiplayerpakken Drake's Fortune",
     "Requer o pacote multijogador Drake's Fortune", "Vereist het multiplayerpakket Drake's Fortune",
     "「エル・ドラドの秘宝」マルチプレイヤーパックが必要です", "Drake's Fortune 멀티플레이어 팩이 필요합니다", "Vaatii Drake's Fortune -moninpelipaketin",
     "Krever flerspillerpakken Drake's Fortune", "Kräver multiplayerpaketet Drake's Fortune",
     "需要「Drake's Fortune」多人遊戲包", "Требуется сетевой набор Drake's Fortune",
     "Wymaga pakietu wieloosobowego Drake's Fortune", nullptr, nullptr,
     "Απαιτεί το πακέτο πολλών παικτών Drake's Fortune", "Vyžaduje balíček pro více hráčů Drake's Fortune",
     "Drake's Fortune Çok Oyunculu Paketi gerekir"}},
    {0xc4c378d1u, {"You don't have a DLC map pack.\\\\Would you like to go to the Store now?",
     "Vous n'avez pas de pack de cartes DLC.\\\\Voulez-vous aller dans la boutique maintenant ?",
     "Non possiedi un pacchetto mappe DLC.\\\\Vuoi andare al negozio ora?",
     "Sie besitzen kein DLC-Kartenpaket.\\\\Möchten Sie jetzt zum Store wechseln?",
     "No tienes ningún paquete de mapas DLC.\\\\¿Quieres ir a la tienda ahora?",
     "Du har ingen DLC-kortpakke.\\\\Vil du gå til butikken nu?",
     "Não tens nenhum pacote de mapas DLC.\\\\Queres ir à loja agora?",
     "Je hebt geen DLC-kaartenpakket.\\\\Wil je nu naar de winkel?", "DLCマップパックを持っていません。\\\\今すぐストアに移動しますか？",
     "DLC 맵 팩이 없습니다.\\\\지금 스토어로 이동하시겠습니까?", "Sinulla ei ole DLC-karttapakettia.\\\\Haluatko siirtyä kauppaan nyt?",
     "Du har ingen DLC-kartpakke.\\\\Vil du gå til butikken nå?",
     "Du har inget DLC-kartpaket.\\\\Vill du gå till butiken nu?", "你沒有 DLC 地圖包。\\\\要立即前往商店嗎？",
     "У вас нет набора карт DLC.\\\\Перейти в магазин?",
     "Nie masz pakietu map DLC.\\\\Czy chcesz teraz przejść do sklepu?", nullptr,
     "Você não tem nenhum pacote de mapas DLC.\\\\Deseja ir à loja agora?",
     "Δεν έχεις πακέτο χαρτών DLC.\\\\Θέλεις να μεταβείς στο κατάστημα τώρα;",
     "Nemáte žádný balíček map DLC.\\\\Chcete nyní přejít do obchodu?",
     "Hiç DLC harita paketin yok.\\\\Şimdi mağazaya gitmek ister misin?"}},
    {0x041a85cau, {
     "Thanks for getting a DLC map pack!\\\\Go to your Profile page to set your matchmaking preference?",
     "Merci d'avoir acheté un pack de cartes DLC !\\\\Aller à la page Profil pour régler vos préférences de recherche de partie ?",
     "Grazie per aver acquistato un pacchetto mappe DLC!\\\\Vuoi andare alla pagina Profilo per impostare le preferenze di reperimento giocatori?",
     "Danke für den Kauf eines DLC-Kartenpakets!\\\\Möchten Sie zur Profilseite wechseln, um Ihre Spielvermittlung einzustellen?",
     "¡Gracias por comprar un paquete de mapas DLC!\\\\¿Quieres ir a tu perfil para ajustar tus preferencias de partida rápida?",
     "Tak, fordi du har købt en DLC-kortpakke!\\\\Vil du gå til din profilside og vælge dine matchmaking-indstillinger?",
     "Obrigado por adquirires um pacote de mapas DLC!\\\\Queres ir à página de Perfil para definir as preferências de correspondência de jogo?",
     "Bedankt voor het kopen van een DLC-kaartenpakket!\\\\Wil je naar je profielpagina om je matchmakingvoorkeur in te stellen?",
     "DLCマップパックをお買い上げいただきありがとうございます！\\\\プロフィール画面でマッチメイクの設定を行いますか？",
     "DLC 맵 팩을 구매해 주셔서 감사합니다!\\\\프로필 페이지로 이동하여 매치메이킹 설정을 하시겠습니까?",
     "Kiitos DLC-karttapaketin hankkimisesta!\\\\Haluatko siirtyä profiilisivulle asettamaan pelien haun asetukset?",
     "Takk for at du skaffet deg en DLC-kartpakke!\\\\Vil du gå til profilsiden din og stille inn matching?",
     "Tack för att du skaffade ett DLC-kartpaket!\\\\Vill du gå till din profilsida och ställa in matchningen?",
     "感謝你購買 DLC 地圖包！\\\\要前往個人資料頁面設定配對偏好嗎？",
     "Спасибо за покупку набора карт DLC!\\\\Перейти на страницу учетной записи, чтобы настроить подбор игроков?",
     "Dziękujemy za zakup pakietu map DLC!\\\\Czy chcesz przejść do profilu, aby ustawić preferencje wyszukiwania graczy?",
     nullptr,
     "Obrigado por adquirir um pacote de mapas DLC!\\\\Deseja ir à página de Perfil para definir sua preferência de busca de partidas?",
     "Ευχαριστούμε που απέκτησες ένα πακέτο χαρτών DLC!\\\\Θέλεις να μεταβείς στο προφίλ σου για να ορίσεις τις προτιμήσεις εύρεσης αντιπάλων;",
     "Děkujeme za pořízení balíčku map DLC!\\\\Chcete přejít na stránku profilu a nastavit vytváření hry?",
     "DLC harita paketi aldığın için teşekkürler!\\\\Eşleştirme tercihini ayarlamak için profil sayfana gitmek ister misin?"}},
    // ---- Siege (co-op) and the DLC maps of net-patch9
    {0x63801125u, {"Guardians", "Gardiens", "Guardiani", "Wächter", "Guardianes", "Vogtere", "Guardiões", "Bewakers",
     "ガーディアン", "수호자", "Vartijat", "Voktere", "Väktare", "守護者", "Стражи", "Strażnicy", nullptr, nullptr, "Φύλακες",
     "Strážci", "Muhafızlar"}},
    {0xc5ca76edu, {"Hold off waves of soldiers to defend your territories.",
     "Repoussez les vagues de soldats pour défendre vos territoires.",
     "Respingi le ondate di soldati per difendere i tuoi territori.",
     "Wehren Sie Wellen von Soldaten ab, um Ihre Gebiete zu verteidigen.",
     "Contén las oleadas de soldados para defender tus territorios.",
     "Afværg bølger af soldater for at forsvare dine territorier.",
     "Trava as vagas de soldados para defender os teus territórios.",
     "Houd golven soldaten tegen om je gebieden te verdedigen.", "押し寄せる兵士の波から陣地を守り抜け。", "몰려오는 병사들로부터 영역을 지켜내십시오.",
     "Torju sotilasaallot ja puolusta alueitasi.",
     "Slå tilbake bølger av soldater for å forsvare territoriene dine.",
     "Slå tillbaka vågor av soldater för att försvara dina territorier.", "抵擋一波波士兵，守住你的區域。",
     "Отражайте волны солдат и удерживайте свои территории.", "Odpieraj fale żołnierzy, by obronić swoje terytoria.",
     nullptr, "Segure as ondas de soldados para defender seus territórios.",
     "Απώθησε τα κύματα στρατιωτών για να υπερασπιστείς τις περιοχές σου.",
     "Odrážejte vlny vojáků a braňte svá území.", "Bölgelerini savunmak için asker dalgalarını püskürt."}},
    {0x23d9e9d0u, {"The Facility", "Le complexe", "La struttura", "Die Anlage", "Las instalaciones", "Anlægget",
     "As Instalações", "The Facility", "研究施設", "시설", "Laitos", "Anlegget", "Anläggningen", "研究設施", "Комплекс",
     "Kompleks", nullptr, nullptr, "Η Εγκατάσταση", "Zařízení", "Tesis"}},
    {0xfc072575u, {"The Highrise", "La tour", "Il grattacielo", "Das Hochhaus", "El rascacielos", "Højhuset",
     "O Arranha-céus", "The Highrise", "高層ビル", "고층 빌딩", "Pilvenpiirtäjä", "Høyhuset", "Höghuset", "摩天大樓", "Высотка",
     "Wieżowiec", nullptr, "O Arranha-céu", "Ο Ουρανοξύστης", "Mrakodrap", "Gökdelen"}},
    {0x2279cab5u, {"The Museum", "Le musée", "Il museo", "Das Museum", "El museo", "Museet", "O Museu", "The Museum",
     "博物館", "박물관", "Museo", "Museet", "Museet", "博物館", "Музей", "Muzeum", nullptr, nullptr, "Το Μουσείο", "Muzeum",
     "Müze"}},
    // ---- Matchmaking playlists (menus.cpp kPlaylistDefs; net-patch9 ids are aliases)
    {kTextAllCompetitive, {"All Competitive Modes", "Tous les modes compétition", "Tutte le modalità competitive",
     "Alle Kompetitiv-Modi", "Todos los modos competitivos", "Alle konkurrencetilstande",
     "Todos os modos competitivos", "Alle competitieve modi", "すべての対戦モード", "모든 경쟁 모드", "Kaikki kilpailutilat",
     "Alle konkurransemoduser", "Alla tävlingslägen", "所有對戰模式", "Все режимы соперничества",
     "Wszystkie tryby rywalizacji", nullptr, nullptr, "Όλες οι ανταγωνιστικές λειτουργίες",
     "Všechny soutěžní režimy", "Tüm Rekabetçi Modlar"}},
    {kTextAllCompetitiveDesc, {"A mix of Deathmatch and Objective game types",
     "Un mélange de matchs à mort et de modes à objectifs", "Un mix di Deathmatch e modalità a obiettivi",
     "Eine Mischung aus Deathmatch- und Ziel-Spieltypen", "Una mezcla de duelos y modos de objetivos",
     "En blanding af deathmatch og målbaserede spiltyper", "Uma mistura de combate mortal e modos de objetivos",
     "Een mix van deathmatch en speltypes met doelen", "デスマッチと目標達成型ゲームの組み合わせ", "데스매치와 목표 게임 유형의 조합",
     "Sekoitus deathmatch- ja tavoitepelejä", "En blanding av dødskamp og målbaserte spilltyper",
     "En blandning av dödsmatch och målbaserade speltyper", "決死戰與目標類遊戲的組合",
     "Схватки и режимы с заданиями вперемешку", "Mieszanka starć drużyn i trybów z celami", nullptr,
     "Uma mistura de combate mortal e modos de objetivo", "Ένας συνδυασμός Deathmatch και παιχνιδιών με στόχους",
     "Mix deathmatche a her s cíli", "Ölüm Maçı ve hedef tabanlı oyun türlerinin karışımı"}},
    {kTextTheLab, {"The Lab", "Le Labo", "Il Laboratorio", "Das Labor", "El Laboratorio", "Laboratoriet",
     "O Laboratório", "Het Lab", "ラボ", "실험실", "Laboratorio", "Laboratoriet", "Labbet", "實驗室", "Лаборатория",
     "Laboratorium", nullptr, nullptr, "Το Εργαστήριο", "Laboratoř", "Laboratuvar"}},
    {kTextTheLabDesc, {"Experimental game types", "Types de jeu expérimentaux", "Modalità di gioco sperimentali",
     "Experimentelle Spieltypen", "Tipos de juego experimentales", "Eksperimentelle spiltyper",
     "Tipos de jogo experimentais", "Experimentele speltypes", "実験的なゲームタイプ", "실험적인 게임 유형",
     "Kokeellisia pelityyppejä", "Eksperimentelle spilltyper", "Experimentella speltyper", "實驗性遊戲類型",
     "Экспериментальные режимы", "Eksperymentalne rodzaje gier", nullptr, nullptr, "Πειραματικοί τύποι παιχνιδιού",
     "Experimentální typy her", "Deneysel oyun türleri"}},
    {kTextEliminationDesc, {"Elimination matches only.", "Uniquement des parties Élimination.",
     "Solo partite Eliminazione.", "Nur Eliminierung-Spiele.", "Solo partidas de Eliminación.",
     "Kun elimineringskampe.", "Apenas partidas de Eliminação.", "Alleen Elimination-potjes.", "チームデスマッチのみ。",
     "제거 경기만.", "Vain eliminointipelejä.", "Kun elimineringskamper.", "Endast utslagningsmatcher.", "僅限淘汰戰。",
     "Только режим «Уничтожение».", "Tylko mecze likwidacji.", nullptr, nullptr, "Μόνο αγώνες αποκλεισμού.",
     "Pouze zápasy v eliminaci.", "Yalnızca Eleme maçları."}},
    {0xe719276eu, {"Standard Deathmatch matches only.", "Uniquement des matchs à mort classiques.",
     "Solo partite Deathmatch standard.", "Nur Standard-Deathmatch.", "Solo duelos estándar.",
     "Kun almindelig deathmatch.", "Apenas combate mortal normal.", "Alleen standaard deathmatch.", "通常のデスマッチのみ。",
     "일반 데스매치만.", "Vain tavallisia deathmatch-pelejä.", "Kun vanlig dødskamp.", "Endast vanlig dödsmatch.",
     "僅限標準決死戰。", "Только обычная «Схватка».", "Tylko standardowe starcia drużyn.", nullptr,
     "Apenas combate mortal padrão.", "Μόνο κανονικό Deathmatch.", "Pouze standardní deathmatch.",
     "Yalnızca standart Ölüm Maçı."}},
    // ---- net-patch9 game type variants
    {0x23bc211fu, {"Shotguns Only", "Fusils à pompe uniquement", "Solo fucili a pompa", "Nur Schrotflinten",
     "Solo escopetas", "Kun haglgeværer", "Só caçadeiras", "Alleen shotguns", "ショットガンのみ", "산탄총만", "Vain haulikot",
     "Kun hagler", "Endast hagelgevär", "僅限霰彈槍", "Только дробовики", "Tylko strzelby", nullptr, "Só espingardas",
     "Μόνο καραμπίνες", "Pouze brokovnice", "Sadece Pompalı Tüfek"}},
    {0xc7e9ab70u, {"Sniper Elimination", "Élimination au sniper", "Eliminazione cecchini",
     "Scharfschützen-Eliminierung", "Eliminación con francotirador", "Snigskytte-eliminering",
     "Eliminação de atiradores", "Sniper-Elimination", "スナイパー・チームデスマッチ", "저격 제거", "Tarkka-ampujaeliminointi",
     "Snikskytter-eliminering", "Prickskytteutslagning", "狙擊淘汰戰", "Уничтожение: снайперы", "Likwidacja snajperska",
     nullptr, "Eliminação de franco-atiradores", "Αποκλεισμός ελεύθερων σκοπευτών", "Odstřelovačská eliminace",
     "Keskin Nişancı Elemesi"}},
    {0x5c96396eu, {"Pistols Only", "Pistolets uniquement", "Solo pistole", "Nur Pistolen", "Solo pistolas",
     "Kun pistoler", "Só pistolas", "Alleen pistolen", "ピストルのみ", "권총만", "Vain pistoolit", "Kun pistoler",
     "Endast pistoler", "僅限手槍", "Только пистолеты", "Tylko pistolety", nullptr, nullptr, "Μόνο πιστόλια",
     "Pouze pistole", "Sadece Tabanca"}},
    {0xb0f9856bu, {"No Power Weapons", "Pas d'armes lourdes", "Niente armi potenti", "Keine Powerwaffen",
     "Sin armas potentes", "Ingen kraftvåben", "Sem armas poderosas", "Geen krachtige wapens", "パワーウェポンなし",
     "파워 무기 없음", "Ei tehoaseita", "Ingen kraftvåpen", "Inga kraftvapen", "無強力武器", "Без мощного оружия",
     "Bez potężnej broni", nullptr, nullptr, "Χωρίς ισχυρά όπλα", "Bez silných zbraní", "Güçlü Silah Yok"}},
    {0xe67358fbu, {"Desert - 5 Only", "Desert - 5 uniquement", "Solo Desert - 5", "Nur Desert (Kaliber 50)",
     "Solo Desert - 5", "Kun Desert - 5", "Só Desert - 5", "Alleen Desert - 5", "Desert - 5のみ", "Desert - 5만",
     "Vain Desert - 5", "Kun Desert - 5", "Endast Desert - 5", "僅限 Desert - 5", "Только «Дезерт Игл»",
     "Tylko Desert-5", nullptr, nullptr, "Μόνο Desert - 5", "Pouze Desert - 5", "Sadece Desert - 5"}},
    {0x3f4b6279u, {"3 vs. 3", "3 contre 3", "3 contro 3", "3 gegen 3", "3 contra 3", "3 mod 3", "3 contra 3",
     "3 tegen 3", "3対3", "3 대 3", "3 vastaan 3", "3 mot 3", "3 mot 3", "3 對 3", "3 на 3", "3 na 3", nullptr, nullptr,
     "3 εναντίον 3", "3 proti 3", "3'e 3"}},
    {0x3a52d349u, {"2 vs. 2", "2 contre 2", "2 contro 2", "2 gegen 2", "2 contra 2", "2 mod 2", "2 contra 2",
     "2 tegen 2", "2対2", "2 대 2", "2 vastaan 2", "2 mot 2", "2 mot 2", "2 對 2", "2 на 2", "2 na 2", nullptr, nullptr,
     "2 εναντίον 2", "2 proti 2", "2'ye 2"}},
    {0x2bd5ef65u, {"No Boosters", "Pas d'optimisations", "Niente potenziamenti", "Keine Booster",
     "Sin potenciadores", "Ingen forstærkere", "Sem turbos", "Geen boosters", "ブースターなし", "부스터 없음", "Ei tehostuksia",
     "Ingen boostere", "Inga boosters", "無助力器", "Без модификаторов", "Bez dopalaczy", nullptr, nullptr,
     "Χωρίς ενισχύσεις", "Bez vylepšení", "Eklenti Yok"}},
    {0x763e0e75u, {"M32 - Hammer Only", "Lance-grenades M32 uniquement", "Solo M32 Hammer", "Nur M32-Hammer",
     "Solo M32 - Hammer", "Kun M32-hammer", "Só M32 - Martelo", "Alleen M32 - Hammer", "M32ハンマーのみ", "M32 - Hammer만",
     "Vain M32 - Hammer", "Kun M32 - Hammer", "Endast M32 - Hammer", "僅限 M32 戰鎚", "Только гранатомет M32 - Hammer",
     "Tylko M32 - Hammer", nullptr, "Só M32 - Hammer", "Μόνο M32 - Hammer", "Pouze M32 - Hammer",
     "Sadece M32 - Hammer"}},
    {0x65ae8cb9u, {"Sniper Plunder", "Mise à sac au sniper", "Saccheggio con cecchini", "Scharfschützen-Plündern",
     "Saqueo con francotirador", "Snigskytte-plyndring", "Saquear com atiradores", "Sniper-Plunder", "スナイパー宝探し",
     "저격 약탈", "Tarkka-ampujaryöstö", "Snikskytter-plyndring", "Prickskytteplundring", "狙擊奪寶戰", "Грабеж: снайперы",
     "Grabież snajperska", nullptr, "Saque com franco-atiradores", "Λεηλασία ελεύθερων σκοπευτών",
     "Odstřelovačské plenění", "Keskin Nişancı Yağması"}},
    // ---- GOTY skins without a name text (net-patch9 skin table 5a4731d2; unlocks_text is the last resort)
    {0xaacd3194u, {"Santa Drake", "Drake Père Noël", "Drake Babbo Natale", "Weihnachtsmann-Drake", "Drake Papá Noel",
     "Julemands-Drake", "Drake Pai Natal", "Kerstman-Drake", "サンタ・ネイト", "산타 드레이크", "Joulupukki-Drake",
     "Julenisse-Drake", "Jultomte-Drake", "聖誕老人德瑞克", "Дрейк-Санта", "Drake Mikołaj", nullptr, "Drake Papai Noel",
     "Drake Άγιος Βασίλης", "Drake Santa Claus", "Noel Baba Drake"}},
    {0x80f54e0fu, {"Doughnut Lazarević", "Lazarević obèse", "Lazarević ciambella", "Donut-Lazarević",
     "Rosquilla Lazarević", "Doughnut-Lazarević", "Lazarević donut", "Donut-Lazarević", "太っちょラザレビッチ", "도넛 라자레비치",
     "Donitsi-Lazarević", "Smultring-Lazarević", "Kakälskaren Lazarević", "肥胖的拉札雷維奇", "Лазаревич-пончик",
     "Pączkowy Lazarević", "Lazarević enrosquillado", "Lazarević Gorducho", "Lazarević με Ντόνατ",
     "Kobliha Lazarević", "Çörekçi Lazarević"}},
    {0x006d7d58u, {"Heist Chloe", "Chloe cambrioleuse", "Chloe ladra", "Chloe als Diebin", "Chloe ladrona",
     "Kup-Chloe", "Assalto Chloe", "Chloe als rover", "強盗犯クロエ", "강도 클로에", "Ryöstäjä-Chloe", "Kupp-Chloe",
     "Kupp-Chloe", "竊盜克洛伊", "Хлоя-грабительница", "Włamywaczka Chloe", nullptr, "Chloe assaltante",
     "Chloe διαρρήκτρια", "Lupička Chloe", "Soyguncu Chloe"}},
    {0xb461f41eu, {"UDF Sullivan", "Sullivan Uncharted : Drake's Fortune", "Sullivan UDF", "Sullivan (UDS)",
     "Sullivan UDF", "UDF-Sullivan", "Sullivan UDF", "UDF-Sullivan", "サリー （エル・ドラドの秘宝）", "Uncharted 1 설리반",
     "UDF-Sullivan", "UDF Sullivan", "UDF-Sullivan", "蘇利文（首部曲時代）", "Салливан (UDF)", "Sullivan UDF", nullptr,
     nullptr, "UDF Sullivan", "Sullivan - UDF", "UDF Sullivan"}},
    {0x25416211u, {"Anniversary Drake", "Drake anniversaire", "Drake anniversario", "Jubiläums-Drake",
     "Drake aniversario", "Jubilæums-Drake", "Drake aniversário", "Jubileum-Drake", "アニバーサリー・ネイト", "기념일 드레이크",
     "Juhla-Drake", "Jubileums-Drake", "Jubileums-Drake", "週年紀念德瑞克", "Юбилейный Дрейк", "Rocznicowy Drake", nullptr,
     nullptr, "Drake επετείου", "Výroční Drake", "Yıl Dönümü Drake"}},
    {0x91436a43u, {"Rika", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
     nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr}},
    {0x11f7b7ecu, {"Pinkerton", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
     nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr}},
    {0x6d76782eu, {"Sev", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
     nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr}},
    {0x8acd02bfu, {"Helghast", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
     nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr}},
    {0x4f19c9beu, {"Nathan Hale", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
     nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr}},
    {0x868de84bu, {"Chimera Hybrid", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
     nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr}},
    {0xfd14e3aeu, {"Zeke", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
     nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr}},
    {0xbe99f3ddu, {"Blain", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
     nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr}},
    {0xb3dad504u, {"Dutch", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
     nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr}},
    {0xb71bc8b3u, {"Mac", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
     nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr}},
    {0xa95c98b6u, {"Dillon", nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
     nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr}},
    // ---- PC message boxes (remaining.cpp)
    {kTextStore, {"The PlayStation Store is not available on PC.",
     "Le PlayStation Store n'est pas disponible sur PC.", "Il PlayStation Store non è disponibile su PC.",
     "Der PlayStation Store ist auf dem PC nicht verfügbar.", "PlayStation Store no está disponible en PC.",
     "PlayStation Store er ikke tilgængelig på pc.", "A PlayStation Store não está disponível no PC.",
     "De PlayStation Store is niet beschikbaar op pc.", "PC版ではPlayStation Storeを利用できません。",
     "PC에서는 PlayStation Store를 이용할 수 없습니다.", "PlayStation Store ei ole käytettävissä PC:llä.",
     "PlayStation Store er ikke tilgjengelig på PC.", "PlayStation Store är inte tillgänglig på PC.",
     "PC 版無法使用 PlayStation Store。", "PlayStation Store недоступен на ПК.",
     "PlayStation Store nie jest dostępny na PC.", nullptr, nullptr,
     "Το PlayStation Store δεν είναι διαθέσιμο στο PC.", "PlayStation Store není na PC k dispozici.",
     "PlayStation Store PC'de kullanılamaz."}},
    {kTextVoice, {"Voice chat is turned off (UNCHARTED_U2MP_VOICE=off).",
     "Le chat vocal est désactivé (UNCHARTED_U2MP_VOICE=off).",
     "La chat vocale è disattivata (UNCHARTED_U2MP_VOICE=off).",
     "Der Sprachchat ist ausgeschaltet (UNCHARTED_U2MP_VOICE=off).",
     "El chat de voz está desactivado (UNCHARTED_U2MP_VOICE=off).",
     "Stemmechat er slået fra (UNCHARTED_U2MP_VOICE=off).",
     "O chat de voz está desligado (UNCHARTED_U2MP_VOICE=off).", "Voicechat staat uit (UNCHARTED_U2MP_VOICE=off).",
     "ボイスチャットはオフになっています（UNCHARTED_U2MP_VOICE=off）。", "음성 채팅이 꺼져 있습니다(UNCHARTED_U2MP_VOICE=off).",
     "Puhechat on pois päältä (UNCHARTED_U2MP_VOICE=off).", "Talechat er slått av (UNCHARTED_U2MP_VOICE=off).",
     "Röstchatten är avstängd (UNCHARTED_U2MP_VOICE=off).", "語音聊天已關閉（UNCHARTED_U2MP_VOICE=off）。",
     "Голосовой чат отключен (UNCHARTED_U2MP_VOICE=off).", "Czat głosowy jest wyłączony (UNCHARTED_U2MP_VOICE=off).",
     nullptr, "O chat de voz está desativado (UNCHARTED_U2MP_VOICE=off).",
     "Η φωνητική συνομιλία είναι απενεργοποιημένη (UNCHARTED_U2MP_VOICE=off).",
     "Hlasový chat je vypnutý (UNCHARTED_U2MP_VOICE=off).", "Sesli sohbet kapalı (UNCHARTED_U2MP_VOICE=off)."}},
    {kTextLeader, {"On PC the player who hosts the party leads it. The leader can't be changed.",
     "Sur PC, le joueur qui héberge le groupe le dirige. Le chef ne peut pas être changé.",
     "Su PC il giocatore che ospita il gruppo ne è il capo. Il capo non può essere cambiato.",
     "Auf dem PC leitet der Spieler die Gruppe, der sie hostet. Der Anführer kann nicht gewechselt werden.",
     "En PC, el jugador que aloja el grupo es su líder. El líder no se puede cambiar.",
     "På pc er den spiller, der er vært for gruppen, leder. Lederen kan ikke skiftes.",
     "No PC, o jogador que aloja o grupo é o líder. O líder não pode ser mudado.",
     "Op pc leidt de speler die de groep host de groep. De leider kan niet worden gewijzigd.",
     "PC版ではパーティーをホストしているプレイヤーがリーダーになります。リーダーは変更できません。", "PC에서는 파티를 호스팅하는 플레이어가 리더입니다. 리더는 변경할 수 없습니다.",
     "PC:llä ryhmää johtaa sen isännöivä pelaaja. Johtajaa ei voi vaihtaa.",
     "På PC leder spilleren som er vert for gruppen den. Lederen kan ikke byttes.",
     "På PC leds gruppen av spelaren som är värd för den. Ledaren kan inte bytas.", "在 PC 版中，主持隊伍的玩家即為隊長。隊長無法變更。",
     "На ПК группой руководит игрок, который ее создал. Лидера сменить нельзя.",
     "Na PC liderem grupy jest gracz, który ją hostuje. Lidera nie można zmienić.", nullptr,
     "No PC, o jogador que hospeda o grupo é o líder. O líder não pode ser trocado.",
     "Στο PC αρχηγός της ομάδας είναι ο παίκτης που τη φιλοξενεί. Ο αρχηγός δεν μπορεί να αλλάξει.",
     "Na PC skupinu vede hráč, který ji hostuje. Vůdce nelze změnit.",
     "PC'de grubu, grubu barındıran oyuncu yönetir. Lider değiştirilemez."}},
    {kTextInvite, {
     "Invites are not used on PC. To join the party, a friend starts the game with UNCHARTED_U2MP_JOIN set to the party leader's address and selects Multiplayer.",
     "Les invitations ne sont pas utilisées sur PC. Pour rejoindre le groupe, un ami lance le jeu avec UNCHARTED_U2MP_JOIN réglé sur l'adresse du chef du groupe et choisit Multijoueur.",
     "Su PC gli inviti non si usano. Per unirsi al gruppo, un amico avvia il gioco con UNCHARTED_U2MP_JOIN impostato sull'indirizzo del capo del gruppo e sceglie Multigiocatore.",
     "Einladungen gibt es auf dem PC nicht. Um der Gruppe beizutreten, startet ein Freund das Spiel mit UNCHARTED_U2MP_JOIN auf die Adresse des Gruppenleiters und wählt Mehrspieler.",
     "En PC no se usan invitaciones. Para unirse al grupo, un amigo inicia el juego con UNCHARTED_U2MP_JOIN igual a la dirección del líder del grupo y elige Multijugador.",
     "Invitationer bruges ikke på pc. For at slutte sig til gruppen starter en ven spillet med UNCHARTED_U2MP_JOIN sat til gruppelederens adresse og vælger Multiplayer.",
     "No PC não se usam convites. Para se juntar ao grupo, um amigo inicia o jogo com UNCHARTED_U2MP_JOIN definido para o endereço do líder do grupo e escolhe Multijogador.",
     "Uitnodigingen worden op pc niet gebruikt. Om bij de groep te komen, start een vriend het spel met UNCHARTED_U2MP_JOIN ingesteld op het adres van de groepsleider en kiest Multiplayer.",
     "PC版では招待を使用しません。パーティーに参加するには、フレンドがUNCHARTED_U2MP_JOINにパーティーリーダーのアドレスを設定してゲームを起動し、マルチプレイを選択します。",
     "PC에서는 초대를 사용하지 않습니다. 파티에 참가하려면 친구가 UNCHARTED_U2MP_JOIN을 파티 리더의 주소로 설정해 게임을 시작한 뒤 멀티 플레이를 선택하면 됩니다.",
     "Kutsuja ei käytetä PC:llä. Liittyäkseen ryhmään ystävä käynnistää pelin niin, että UNCHARTED_U2MP_JOIN on ryhmän johtajan osoite, ja valitsee Moninpeli.",
     "Invitasjoner brukes ikke på PC. For å bli med i gruppen starter en venn spillet med UNCHARTED_U2MP_JOIN satt til gruppelederens adresse og velger Flerspiller.",
     "Inbjudningar används inte på PC. För att gå med i gruppen startar en vän spelet med UNCHARTED_U2MP_JOIN satt till gruppledarens adress och väljer Fler spelare.",
     "PC 版不使用邀請。好友若要加入隊伍，請將 UNCHARTED_U2MP_JOIN 設為隊長的位址後啟動遊戲，然後選擇多人遊玩。",
     "На ПК приглашения не используются. Чтобы присоединиться к группе, друг запускает игру с UNCHARTED_U2MP_JOIN, равным адресу лидера группы, и выбирает «Коллективная игра».",
     "Zaproszenia nie są używane na PC. Aby dołączyć do grupy, znajomy uruchamia grę z UNCHARTED_U2MP_JOIN ustawionym na adres lidera grupy i wybiera Tryb wieloosobowy.",
     nullptr,
     "Convites não são usados no PC. Para entrar no grupo, um amigo inicia o jogo com UNCHARTED_U2MP_JOIN definido como o endereço do líder do grupo e seleciona Multijogador.",
     "Οι προσκλήσεις δεν χρησιμοποιούνται στο PC. Για να μπει στην ομάδα, ένας φίλος ξεκινά το παιχνίδι με το UNCHARTED_U2MP_JOIN ορισμένο στη διεύθυνση του αρχηγού της ομάδας και επιλέγει Πολλοί παίκτες.",
     "Na PC se pozvánky nepoužívají. Pro připojení ke skupině spustí přítel hru s UNCHARTED_U2MP_JOIN nastaveným na adresu vůdce skupiny a vybere Hra více hráčů.",
     "PC'de davetler kullanılmaz. Gruba katılmak için bir arkadaşın, UNCHARTED_U2MP_JOIN değerini grup liderinin adresi yaparak oyunu başlatır ve Çok Oyunculu'yu seçer."}},
    {kTextClanTag, {"Clan tags are not available on PC.", "Les tags de clan ne sont pas disponibles sur PC.",
     "I tag del clan non sono disponibili su PC.", "Clan-Tags sind auf dem PC nicht verfügbar.",
     "Las etiquetas de clan no están disponibles en PC.", "Klan-tags er ikke tilgængelige på pc.",
     "As etiquetas de clã não estão disponíveis no PC.", "Clantags zijn niet beschikbaar op pc.",
     "PC版ではクランタグを利用できません。", "PC에서는 클랜 태그를 사용할 수 없습니다.", "Klaanitunnisteet eivät ole käytettävissä PC:llä.",
     "Klanmerker er ikke tilgjengelige på PC.", "Klantaggar är inte tillgängliga på PC.", "PC 版無法使用戰隊標籤。",
     "Теги кланов недоступны на ПК.", "Tagi klanów nie są dostępne na PC.", nullptr,
     "As tags de clã não estão disponíveis no PC.", "Οι ετικέτες κλαν δεν είναι διαθέσιμες στο PC.",
     "Klanové značky nejsou na PC k dispozici.", "Klan etiketleri PC'de kullanılamaz."}},
    {kTextTwitter, {"Twitter is not available on PC.", "Twitter n'est pas disponible sur PC.",
     "Twitter non è disponibile su PC.", "Twitter ist auf dem PC nicht verfügbar.",
     "Twitter no está disponible en PC.", "Twitter er ikke tilgængelig på pc.",
     "O Twitter não está disponível no PC.", "Twitter is niet beschikbaar op pc.", "PC版ではTwitterを利用できません。",
     "PC에서는 Twitter를 이용할 수 없습니다.", "Twitter ei ole käytettävissä PC:llä.", "Twitter er ikke tilgjengelig på PC.",
     "Twitter är inte tillgängligt på PC.", "PC 版無法使用 Twitter。", "Twitter недоступен на ПК.",
     "Twitter nie jest dostępny na PC.", nullptr, nullptr, "Το Twitter δεν είναι διαθέσιμο στο PC.",
     "Twitter není na PC k dispozici.", "Twitter PC'de kullanılamaz."}},
    {kTextCinemaSend, {"Sending cinema files to friends is not available on PC.",
     "L'envoi de fichiers cinéma à des amis n'est pas disponible sur PC.",
     "L'invio di file cinema agli amici non è disponibile su PC.",
     "Das Senden von Kino-Dateien an Freunde ist auf dem PC nicht verfügbar.",
     "El envío de archivos de cine a amigos no está disponible en PC.",
     "Det er ikke muligt at sende biograffiler til venner på pc.",
     "O envio de ficheiros de cinema a amigos não está disponível no PC.",
     "Bioscoopbestanden naar vrienden sturen is niet beschikbaar op pc.", "PC版ではシネマファイルをフレンドに送信できません。",
     "PC에서는 친구에게 시네마 파일을 보낼 수 없습니다.", "Elokuvatiedostojen lähettäminen ystäville ei ole käytettävissä PC:llä.",
     "Sending av kinofiler til venner er ikke tilgjengelig på PC.",
     "Det går inte att skicka biofiler till vänner på PC.", "PC 版無法傳送影片檔案給好友。",
     "Отправка файлов роликов друзьям недоступна на ПК.", "Wysyłanie plików kina znajomym nie jest dostępne na PC.",
     nullptr, "O envio de arquivos de cinema para amigos não está disponível no PC.",
     "Η αποστολή αρχείων κινηματογράφου σε φίλους δεν είναι διαθέσιμη στο PC.",
     "Odesílání souborů biografu přátelům není na PC k dispozici.",
     "Arkadaşlara sinema dosyası gönderme PC'de kullanılamaz."}},
    {kTextCinemaReceive, {"Receiving cinema files from friends is not available on PC.",
     "La réception de fichiers cinéma d'amis n'est pas disponible sur PC.",
     "La ricezione di file cinema dagli amici non è disponibile su PC.",
     "Das Empfangen von Kino-Dateien von Freunden ist auf dem PC nicht verfügbar.",
     "La recepción de archivos de cine de amigos no está disponible en PC.",
     "Det er ikke muligt at modtage biograffiler fra venner på pc.",
     "A receção de ficheiros de cinema de amigos não está disponível no PC.",
     "Bioscoopbestanden van vrienden ontvangen is niet beschikbaar op pc.", "PC版ではフレンドからシネマファイルを受信できません。",
     "PC에서는 친구의 시네마 파일을 받을 수 없습니다.", "Elokuvatiedostojen vastaanottaminen ystäviltä ei ole käytettävissä PC:llä.",
     "Mottak av kinofiler fra venner er ikke tilgjengelig på PC.",
     "Det går inte att ta emot biofiler från vänner på PC.", "PC 版無法接收好友的影片檔案。",
     "Получение файлов роликов от друзей недоступно на ПК.",
     "Odbieranie plików kina od znajomych nie jest dostępne na PC.", nullptr,
     "O recebimento de arquivos de cinema de amigos não está disponível no PC.",
     "Η λήψη αρχείων κινηματογράφου από φίλους δεν είναι διαθέσιμη στο PC.",
     "Přijímání souborů biografu od přátel není na PC k dispozici.",
     "Arkadaşlardan sinema dosyası alma PC'de kullanılamaz."}},
    {kTextYou, {"(you)", "(vous)", "(tu)", "(Sie)", "(tú)", "(dig)", "(tu)", "(jij)", "（あなた）", "(나)", "(sinä)",
     "(deg)", "(du)", "（你）", "(вы)", "(ty)", nullptr, "(você)", "(εσύ)", "(vy)", "(sen)"}},
    {kTextMuted, {"muted", "son coupé", "audio disattivato", "stummgeschaltet", "silenciado", "lyd slået fra",
     "silenciado", "gedempt", "ミュート中", "음소거됨", "mykistetty", "dempet", "ljud av", "已靜音", "звук отключен",
     "wyciszony", nullptr, nullptr, "σε σίγαση", "ztlumen", "sessiz"}},
    {kTextUnmuted, {"unmuted", "son activé", "audio attivato", "Stummschaltung aufgehoben", "con sonido",
     "lyd slået til", "com som", "niet gedempt", "ミュート解除", "음소거 해제됨", "mykistys poistettu", "ikke dempet", "ljud på",
     "已取消靜音", "звук включен", "wyciszenie wyłączone", nullptr, "som ativado", "χωρίς σίγαση", "ztlumení zrušeno",
     "sesi açık"}},
    {kTextDeclineRequest, {"Decline Friend Request", "Refuser la demande d'ami", "Rifiuta richiesta di amicizia",
     "Freundschaftsanfrage ablehnen", "Rechazar solicitud de amistad"}},
    {kTextCancelRequest, {"Cancel Friend Request", "Annuler la demande d'ami", "Annulla richiesta di amicizia",
     "Freundschaftsanfrage zurückziehen", "Cancelar solicitud de amistad"}},
    {kTextRemoveFriend, {"Remove Friend", "Retirer l'ami", "Rimuovi amico", "Freund entfernen", "Eliminar amigo"}},
    // ---- GOTY soccer skins (net-patch9 skin table)
    {0xafb61295u, {"Soccer Drake (Argentina)", "Drake footballeur (Argentine)", "Drake calciatore (Argentina)",
     "Fußball-Drake (Argentinien)", "Drake futbolista (Argentina)", "Fodbold-Drake (Argentina)",
     "Drake futebolista (Argentina)", "Voetbal-Drake (Argentinië)", "サッカーネイト（アルゼンチン）", "축구 드레이크 (아르헨티나)",
     "Jalkapallo-Drake (Argentiina)", "Fotball-Drake (Argentina)", "Fotbolls-Drake (Argentina)", "足球德瑞克（阿根廷）",
     "Дрейк-футболист (Аргентина)", "Drake piłkarz (Argentyna)", nullptr, "Drake do futebol (Argentina)",
     "Drake ποδοσφαιριστής (Αργεντινή)", "Drake fotbalista (Argentina)", "Futbolcu Drake (Arjantin)"}},
    {0xb759aaaeu, {"Soccer Drake (Brazil)", "Drake footballeur (Brésil)", "Drake calciatore (Brasile)",
     "Fußball-Drake (Brasilien)", "Drake futbolista (Brasil)", "Fodbold-Drake (Brasilien)",
     "Drake futebolista (Brasil)", "Voetbal-Drake (Brazilië)", "サッカーネイト（ブラジル）", "축구 드레이크 (브라질)",
     "Jalkapallo-Drake (Brasilia)", "Fotball-Drake (Brasil)", "Fotbolls-Drake (Brasilien)", "足球德瑞克（巴西）",
     "Дрейк-футболист (Бразилия)", "Drake piłkarz (Brazylia)", nullptr, "Drake do futebol (Brasil)",
     "Drake ποδοσφαιριστής (Βραζιλία)", "Drake fotbalista (Brazílie)", "Futbolcu Drake (Brezilya)"}},
    {0x2d04dbb1u, {"Soccer Drake (Chile)", "Drake footballeur (Chili)", "Drake calciatore (Cile)",
     "Fußball-Drake (Chile)", "Drake futbolista (Chile)", "Fodbold-Drake (Chile)", "Drake futebolista (Chile)",
     "Voetbal-Drake (Chili)", "サッカーネイト（チリ）", "축구 드레이크 (칠레)", "Jalkapallo-Drake (Chile)", "Fotball-Drake (Chile)",
     "Fotbolls-Drake (Chile)", "足球德瑞克（智利）", "Дрейк-футболист (Чили)", "Drake piłkarz (Chile)", nullptr,
     "Drake do futebol (Chile)", "Drake ποδοσφαιριστής (Χιλή)", "Drake fotbalista (Chile)", "Futbolcu Drake (Şili)"}},
    {0x77e95961u, {"Soccer Drake (England)", "Drake footballeur (Angleterre)", "Drake calciatore (Inghilterra)",
     "Fußball-Drake (England)", "Drake futbolista (Inglaterra)", "Fodbold-Drake (England)",
     "Drake futebolista (Inglaterra)", "Voetbal-Drake (Engeland)", "サッカーネイト（イングランド）", "축구 드레이크 (잉글랜드)",
     "Jalkapallo-Drake (Englanti)", "Fotball-Drake (England)", "Fotbolls-Drake (England)", "足球德瑞克（英格蘭）",
     "Дрейк-футболист (Англия)", "Drake piłkarz (Anglia)", nullptr, "Drake do futebol (Inglaterra)",
     "Drake ποδοσφαιριστής (Αγγλία)", "Drake fotbalista (Anglie)", "Futbolcu Drake (İngiltere)"}},
    {0xd72facf5u, {"Soccer Drake (Germany)", "Drake footballeur (Allemagne)", "Drake calciatore (Germania)",
     "Fußball-Drake (Deutschland)", "Drake futbolista (Alemania)", "Fodbold-Drake (Tyskland)",
     "Drake futebolista (Alemanha)", "Voetbal-Drake (Duitsland)", "サッカーネイト（ドイツ）", "축구 드레이크 (독일)",
     "Jalkapallo-Drake (Saksa)", "Fotball-Drake (Tyskland)", "Fotbolls-Drake (Tyskland)", "足球德瑞克（德國）",
     "Дрейк-футболист (Германия)", "Drake piłkarz (Niemcy)", nullptr, "Drake do futebol (Alemanha)",
     "Drake ποδοσφαιριστής (Γερμανία)", "Drake fotbalista (Německo)", "Futbolcu Drake (Almanya)"}},
    {0xb0c3d4d9u, {"Soccer Drake (Ghana)", "Drake footballeur (Ghana)", "Drake calciatore (Ghana)",
     "Fußball-Drake (Ghana)", "Drake futbolista (Ghana)", "Fodbold-Drake (Ghana)", "Drake futebolista (Gana)",
     "Voetbal-Drake (Ghana)", "サッカーネイト（ガーナ）", "축구 드레이크 (가나)", "Jalkapallo-Drake (Ghana)", "Fotball-Drake (Ghana)",
     "Fotbolls-Drake (Ghana)", "足球德瑞克（迦納）", "Дрейк-футболист (Гана)", "Drake piłkarz (Ghana)", nullptr,
     "Drake do futebol (Gana)", "Drake ποδοσφαιριστής (Γκάνα)", "Drake fotbalista (Ghana)", "Futbolcu Drake (Gana)"}},
    {0x93f6cc91u, {"Soccer Drake (Japan)", "Drake footballeur (Japon)", "Drake calciatore (Giappone)",
     "Fußball-Drake (Japan)", "Drake futbolista (Japón)", "Fodbold-Drake (Japan)", "Drake futebolista (Japão)",
     "Voetbal-Drake (Japan)", "サッカーネイト（日本）", "축구 드레이크 (일본)", "Jalkapallo-Drake (Japani)", "Fotball-Drake (Japan)",
     "Fotbolls-Drake (Japan)", "足球德瑞克（日本）", "Дрейк-футболист (Япония)", "Drake piłkarz (Japonia)", nullptr,
     "Drake do futebol (Japão)", "Drake ποδοσφαιριστής (Ιαπωνία)", "Drake fotbalista (Japonsko)",
     "Futbolcu Drake (Japonya)"}},
    {0xf5d14715u, {"Soccer Drake (Mexico)", "Drake footballeur (Mexique)", "Drake calciatore (Messico)",
     "Fußball-Drake (Mexiko)", "Drake futbolista (México)", "Fodbold-Drake (Mexico)", "Drake futebolista (México)",
     "Voetbal-Drake (Mexico)", "サッカーネイト（メキシコ）", "축구 드레이크 (멕시코)", "Jalkapallo-Drake (Meksiko)",
     "Fotball-Drake (Mexico)", "Fotbolls-Drake (Mexiko)", "足球德瑞克（墨西哥）", "Дрейк-футболист (Мексика)",
     "Drake piłkarz (Meksyk)", nullptr, "Drake do futebol (México)", "Drake ποδοσφαιριστής (Μεξικό)",
     "Drake fotbalista (Mexiko)", "Futbolcu Drake (Meksika)"}},
    {0xf2afce38u, {"Soccer Drake (Netherlands)", "Drake footballeur (Pays-Bas)", "Drake calciatore (Paesi Bassi)",
     "Fußball-Drake (Niederlande)", "Drake futbolista (Países Bajos)", "Fodbold-Drake (Holland)",
     "Drake futebolista (Países Baixos)", "Voetbal-Drake (Nederland)", "サッカーネイト（オランダ）", "축구 드레이크 (네덜란드)",
     "Jalkapallo-Drake (Alankomaat)", "Fotball-Drake (Nederland)", "Fotbolls-Drake (Nederländerna)", "足球德瑞克（荷蘭）",
     "Дрейк-футболист (Нидерланды)", "Drake piłkarz (Holandia)", nullptr, "Drake do futebol (Holanda)",
     "Drake ποδοσφαιριστής (Ολλανδία)", "Drake fotbalista (Nizozemsko)", "Futbolcu Drake (Hollanda)"}},
    {0xfcc3e2f8u, {"Soccer Drake (Paraguay)", "Drake footballeur (Paraguay)", "Drake calciatore (Paraguay)",
     "Fußball-Drake (Paraguay)", "Drake futbolista (Paraguay)", "Fodbold-Drake (Paraguay)",
     "Drake futebolista (Paraguai)", "Voetbal-Drake (Paraguay)", "サッカーネイト（パラグアイ）", "축구 드레이크 (파라과이)",
     "Jalkapallo-Drake (Paraguay)", "Fotball-Drake (Paraguay)", "Fotbolls-Drake (Paraguay)", "足球德瑞克（巴拉圭）",
     "Дрейк-футболист (Парагвай)", "Drake piłkarz (Paragwaj)", nullptr, "Drake do futebol (Paraguai)",
     "Drake ποδοσφαιριστής (Παραγουάη)", "Drake fotbalista (Paraguay)", "Futbolcu Drake (Paraguay)"}},
    {0xad01ddc9u, {"Soccer Drake (Portugal)", "Drake footballeur (Portugal)", "Drake calciatore (Portogallo)",
     "Fußball-Drake (Portugal)", "Drake futbolista (Portugal)", "Fodbold-Drake (Portugal)",
     "Drake futebolista (Portugal)", "Voetbal-Drake (Portugal)", "サッカーネイト（ポルトガル）", "축구 드레이크 (포르투갈)",
     "Jalkapallo-Drake (Portugali)", "Fotball-Drake (Portugal)", "Fotbolls-Drake (Portugal)", "足球德瑞克（葡萄牙）",
     "Дрейк-футболист (Португалия)", "Drake piłkarz (Portugalia)", nullptr, "Drake do futebol (Portugal)",
     "Drake ποδοσφαιριστής (Πορτογαλία)", "Drake fotbalista (Portugalsko)", "Futbolcu Drake (Portekiz)"}},
    {0xae1eed4cu, {"Soccer Drake (South Korea)", "Drake footballeur (Corée du Sud)",
     "Drake calciatore (Corea del Sud)", "Fußball-Drake (Südkorea)", "Drake futbolista (Corea del Sur)",
     "Fodbold-Drake (Sydkorea)", "Drake futebolista (Coreia do Sul)", "Voetbal-Drake (Zuid-Korea)", "サッカーネイト（韓国）",
     "축구 드레이크 (대한민국)", "Jalkapallo-Drake (Etelä-Korea)", "Fotball-Drake (Sør-Korea)", "Fotbolls-Drake (Sydkorea)",
     "足球德瑞克（南韓）", "Дрейк-футболист (Южная Корея)", "Drake piłkarz (Korea Południowa)", nullptr,
     "Drake do futebol (Coreia do Sul)", "Drake ποδοσφαιριστής (Νότια Κορέα)", "Drake fotbalista (Jižní Korea)",
     "Futbolcu Drake (Güney Kore)"}},
    {0xe84a09b4u, {"Soccer Drake (Slovakia)", "Drake footballeur (Slovaquie)", "Drake calciatore (Slovacchia)",
     "Fußball-Drake (Slowakei)", "Drake futbolista (Eslovaquia)", "Fodbold-Drake (Slovakiet)",
     "Drake futebolista (Eslováquia)", "Voetbal-Drake (Slowakije)", "サッカーネイト（スロバキア）", "축구 드레이크 (슬로바키아)",
     "Jalkapallo-Drake (Slovakia)", "Fotball-Drake (Slovakia)", "Fotbolls-Drake (Slovakien)", "足球德瑞克（斯洛伐克）",
     "Дрейк-футболист (Словакия)", "Drake piłkarz (Słowacja)", nullptr, "Drake do futebol (Eslováquia)",
     "Drake ποδοσφαιριστής (Σλοβακία)", "Drake fotbalista (Slovensko)", "Futbolcu Drake (Slovakya)"}},
    {0x129b2f38u, {"Soccer Drake (Spain)", "Drake footballeur (Espagne)", "Drake calciatore (Spagna)",
     "Fußball-Drake (Spanien)", "Drake futbolista (España)", "Fodbold-Drake (Spanien)",
     "Drake futebolista (Espanha)", "Voetbal-Drake (Spanje)", "サッカーネイト（スペイン）", "축구 드레이크 (스페인)",
     "Jalkapallo-Drake (Espanja)", "Fotball-Drake (Spania)", "Fotbolls-Drake (Spanien)", "足球德瑞克（西班牙）",
     "Дрейк-футболист (Испания)", "Drake piłkarz (Hiszpania)", nullptr, "Drake do futebol (Espanha)",
     "Drake ποδοσφαιριστής (Ισπανία)", "Drake fotbalista (Španělsko)", "Futbolcu Drake (İspanya)"}},
    {0xf0cd88e7u, {"Soccer Drake (Uruguay)", "Drake footballeur (Uruguay)", "Drake calciatore (Uruguay)",
     "Fußball-Drake (Uruguay)", "Drake futbolista (Uruguay)", "Fodbold-Drake (Uruguay)",
     "Drake futebolista (Uruguai)", "Voetbal-Drake (Uruguay)", "サッカーネイト（ウルグアイ）", "축구 드레이크 (우루과이)",
     "Jalkapallo-Drake (Uruguay)", "Fotball-Drake (Uruguay)", "Fotbolls-Drake (Uruguay)", "足球德瑞克（烏拉圭）",
     "Дрейк-футболист (Уругвай)", "Drake piłkarz (Urugwaj)", nullptr, "Drake do futebol (Uruguai)",
     "Drake ποδοσφαιριστής (Ουρουγουάη)", "Drake fotbalista (Uruguay)", "Futbolcu Drake (Uruguay)"}},
    {0x7dc1e497u, {"Soccer Drake (USA)", "Drake footballeur (États-Unis)", "Drake calciatore (USA)",
     "Fußball-Drake (USA)", "Drake futbolista (EE. UU.)", "Fodbold-Drake (USA)", "Drake futebolista (EUA)",
     "Voetbal-Drake (VS)", "サッカーネイト（アメリカ）", "축구 드레이크 (미국)", "Jalkapallo-Drake (Yhdysvallat)", "Fotball-Drake (USA)",
     "Fotbolls-Drake (USA)", "足球德瑞克（美國）", "Дрейк-футболист (США)", "Drake piłkarz (USA)", nullptr,
     "Drake do futebol (EUA)", "Drake ποδοσφαιριστής (ΗΠΑ)", "Drake fotbalista (USA)", "Futbolcu Drake (ABD)"}},
    {0x931aed33u, {"Soccer Flynn (Argentina)", "Flynn footballeur (Argentine)", "Flynn calciatore (Argentina)",
     "Fußball-Flynn (Argentinien)", "Flynn futbolista (Argentina)", "Fodbold-Flynn (Argentina)",
     "Flynn futebolista (Argentina)", "Voetbal-Flynn (Argentinië)", "サッカーフリン（アルゼンチン）", "축구 플린 (아르헨티나)",
     "Jalkapallo-Flynn (Argentiina)", "Fotball-Flynn (Argentina)", "Fotbolls-Flynn (Argentina)", "足球費恩（阿根廷）",
     "Флинн-футболист (Аргентина)", "Flynn piłkarz (Argentyna)", nullptr, "Flynn do futebol (Argentina)",
     "Flynn ποδοσφαιριστής (Αργεντινή)", "Flynn fotbalista (Argentina)", "Futbolcu Flynn (Arjantin)"}},
    {0x8bf55508u, {"Soccer Flynn (Brazil)", "Flynn footballeur (Brésil)", "Flynn calciatore (Brasile)",
     "Fußball-Flynn (Brasilien)", "Flynn futbolista (Brasil)", "Fodbold-Flynn (Brasilien)",
     "Flynn futebolista (Brasil)", "Voetbal-Flynn (Brazilië)", "サッカーフリン（ブラジル）", "축구 플린 (브라질)",
     "Jalkapallo-Flynn (Brasilia)", "Fotball-Flynn (Brasil)", "Fotbolls-Flynn (Brasilien)", "足球費恩（巴西）",
     "Флинн-футболист (Бразилия)", "Flynn piłkarz (Brazylia)", nullptr, "Flynn do futebol (Brasil)",
     "Flynn ποδοσφαιριστής (Βραζιλία)", "Flynn fotbalista (Brazílie)", "Futbolcu Flynn (Brezilya)"}},
    {0xe91fd915u, {"Soccer Flynn (Chile)", "Flynn footballeur (Chili)", "Flynn calciatore (Cile)",
     "Fußball-Flynn (Chile)", "Flynn futbolista (Chile)", "Fodbold-Flynn (Chile)", "Flynn futebolista (Chile)",
     "Voetbal-Flynn (Chili)", "サッカーフリン（チリ）", "축구 플린 (칠레)", "Jalkapallo-Flynn (Chile)", "Fotball-Flynn (Chile)",
     "Fotbolls-Flynn (Chile)", "足球費恩（智利）", "Флинн-футболист (Чили)", "Flynn piłkarz (Chile)", nullptr,
     "Flynn do futebol (Chile)", "Flynn ποδοσφαιριστής (Χιλή)", "Flynn fotbalista (Chile)", "Futbolcu Flynn (Şili)"}},
    {0x4b45a6c7u, {"Soccer Flynn (England)", "Flynn footballeur (Angleterre)", "Flynn calciatore (Inghilterra)",
     "Fußball-Flynn (England)", "Flynn futbolista (Inglaterra)", "Fodbold-Flynn (England)",
     "Flynn futebolista (Inglaterra)", "Voetbal-Flynn (Engeland)", "サッカーフリン（イングランド）", "축구 플린 (잉글랜드)",
     "Jalkapallo-Flynn (Englanti)", "Fotball-Flynn (England)", "Fotbolls-Flynn (England)", "足球費恩（英格蘭）",
     "Флинн-футболист (Англия)", "Flynn piłkarz (Anglia)", nullptr, "Flynn do futebol (Inglaterra)",
     "Flynn ποδοσφαιριστής (Αγγλία)", "Flynn fotbalista (Anglie)", "Futbolcu Flynn (İngiltere)"}},
    {0xeb835353u, {"Soccer Flynn (Germany)", "Flynn footballeur (Allemagne)", "Flynn calciatore (Germania)",
     "Fußball-Flynn (Deutschland)", "Flynn futbolista (Alemania)", "Fodbold-Flynn (Tyskland)",
     "Flynn futebolista (Alemanha)", "Voetbal-Flynn (Duitsland)", "サッカーフリン（ドイツ）", "축구 플린 (독일)",
     "Jalkapallo-Flynn (Saksa)", "Fotball-Flynn (Tyskland)", "Fotbolls-Flynn (Tyskland)", "足球費恩（德國）",
     "Флинн-футболист (Германия)", "Flynn piłkarz (Niemcy)", nullptr, "Flynn do futebol (Alemanha)",
     "Flynn ποδοσφαιριστής (Γερμανία)", "Flynn fotbalista (Německo)", "Futbolcu Flynn (Almanya)"}},
    {0x74d8d67du, {"Soccer Flynn (Ghana)", "Flynn footballeur (Ghana)", "Flynn calciatore (Ghana)",
     "Fußball-Flynn (Ghana)", "Flynn futbolista (Ghana)", "Fodbold-Flynn (Ghana)", "Flynn futebolista (Gana)",
     "Voetbal-Flynn (Ghana)", "サッカーフリン（ガーナ）", "축구 플린 (가나)", "Jalkapallo-Flynn (Ghana)", "Fotball-Flynn (Ghana)",
     "Fotbolls-Flynn (Ghana)", "足球費恩（迦納）", "Флинн-футболист (Гана)", "Flynn piłkarz (Ghana)", nullptr,
     "Flynn do futebol (Gana)", "Flynn ποδοσφαιριστής (Γκάνα)", "Flynn fotbalista (Ghana)", "Futbolcu Flynn (Gana)"}},
    {0xaf5a3337u, {"Soccer Flynn (Japan)", "Flynn footballeur (Japon)", "Flynn calciatore (Giappone)",
     "Fußball-Flynn (Japan)", "Flynn futbolista (Japón)", "Fodbold-Flynn (Japan)", "Flynn futebolista (Japão)",
     "Voetbal-Flynn (Japan)", "サッカーフリン（日本）", "축구 플린 (일본)", "Jalkapallo-Flynn (Japani)", "Fotball-Flynn (Japan)",
     "Fotbolls-Flynn (Japan)", "足球費恩（日本）", "Флинн-футболист (Япония)", "Flynn piłkarz (Japonia)", nullptr,
     "Flynn do futebol (Japão)", "Flynn ποδοσφαιριστής (Ιαπωνία)", "Flynn fotbalista (Japonsko)",
     "Futbolcu Flynn (Japonya)"}},
    {0xc97db8b3u, {"Soccer Flynn (Mexico)", "Flynn footballeur (Mexique)", "Flynn calciatore (Messico)",
     "Fußball-Flynn (Mexiko)", "Flynn futbolista (México)", "Fodbold-Flynn (Mexico)", "Flynn futebolista (México)",
     "Voetbal-Flynn (Mexico)", "サッカーフリン（メキシコ）", "축구 플린 (멕시코)", "Jalkapallo-Flynn (Meksiko)",
     "Fotball-Flynn (Mexico)", "Fotbolls-Flynn (Mexiko)", "足球費恩（墨西哥）", "Флинн-футболист (Мексика)",
     "Flynn piłkarz (Meksyk)", nullptr, "Flynn do futebol (México)", "Flynn ποδοσφαιριστής (Μεξικό)",
     "Flynn fotbalista (Mexiko)", "Futbolcu Flynn (Meksika)"}},
    {0xbf6e9eccu, {"Soccer Flynn (Netherlands)", "Flynn footballeur (Pays-Bas)", "Flynn calciatore (Paesi Bassi)",
     "Fußball-Flynn (Niederlande)", "Flynn futbolista (Países Bajos)", "Fodbold-Flynn (Holland)",
     "Flynn futebolista (Países Baixos)", "Voetbal-Flynn (Nederland)", "サッカーフリン（オランダ）", "축구 플린 (네덜란드)",
     "Jalkapallo-Flynn (Alankomaat)", "Fotball-Flynn (Nederland)", "Fotbolls-Flynn (Nederländerna)", "足球費恩（荷蘭）",
     "Флинн-футболист (Нидерланды)", "Flynn piłkarz (Holandia)", nullptr, "Flynn do futebol (Holanda)",
     "Flynn ποδοσφαιριστής (Ολλανδία)", "Flynn fotbalista (Nizozemsko)", "Futbolcu Flynn (Hollanda)"}},
    {0xb102b20cu, {"Soccer Flynn (Paraguay)", "Flynn footballeur (Paraguay)", "Flynn calciatore (Paraguay)",
     "Fußball-Flynn (Paraguay)", "Flynn futbolista (Paraguay)", "Fodbold-Flynn (Paraguay)",
     "Flynn futebolista (Paraguai)", "Voetbal-Flynn (Paraguay)", "サッカーフリン（パラグアイ）", "축구 플린 (파라과이)",
     "Jalkapallo-Flynn (Paraguay)", "Fotball-Flynn (Paraguay)", "Fotbolls-Flynn (Paraguay)", "足球費恩（巴拉圭）",
     "Флинн-футболист (Парагвай)", "Flynn piłkarz (Paragwaj)", nullptr, "Flynn do futebol (Paraguai)",
     "Flynn ποδοσφαιριστής (Παραγουάη)", "Flynn fotbalista (Paraguay)", "Futbolcu Flynn (Paraguay)"}},
    {0xe0c08d3du, {"Soccer Flynn (Portugal)", "Flynn footballeur (Portugal)", "Flynn calciatore (Portogallo)",
     "Fußball-Flynn (Portugal)", "Flynn futbolista (Portugal)", "Fodbold-Flynn (Portugal)",
     "Flynn futebolista (Portugal)", "Voetbal-Flynn (Portugal)", "サッカーフリン（ポルトガル）", "축구 플린 (포르투갈)",
     "Jalkapallo-Flynn (Portugali)", "Fotball-Flynn (Portugal)", "Fotbolls-Flynn (Portugal)", "足球費恩（葡萄牙）",
     "Флинн-футболист (Португалия)", "Flynn piłkarz (Portugalia)", nullptr, "Flynn do futebol (Portugal)",
     "Flynn ποδοσφαιριστής (Πορτογαλία)", "Flynn fotbalista (Portugalsko)", "Futbolcu Flynn (Portekiz)"}},
    {0xb441dabeu, {"Soccer Flynn (South Korea)", "Flynn footballeur (Corée du Sud)",
     "Flynn calciatore (Corea del Sud)", "Fußball-Flynn (Südkorea)", "Flynn futbolista (Corea del Sur)",
     "Fodbold-Flynn (Sydkorea)", "Flynn futebolista (Coreia do Sul)", "Voetbal-Flynn (Zuid-Korea)", "サッカーフリン（韓国）",
     "축구 플린 (대한민국)", "Jalkapallo-Flynn (Etelä-Korea)", "Fotball-Flynn (Sør-Korea)", "Fotbolls-Flynn (Sydkorea)",
     "足球費恩（南韓）", "Флинн-футболист (Южная Корея)", "Flynn piłkarz (Korea Południowa)", nullptr,
     "Flynn do futebol (Coreia do Sul)", "Flynn ποδοσφαιριστής (Νότια Κορέα)", "Flynn fotbalista (Jižní Korea)",
     "Futbolcu Flynn (Güney Kore)"}},
    {0xd4e6f612u, {"Soccer Flynn (Slovakia)", "Flynn footballeur (Slovaquie)", "Flynn calciatore (Slovacchia)",
     "Fußball-Flynn (Slowakei)", "Flynn futbolista (Eslovaquia)", "Fodbold-Flynn (Slovakiet)",
     "Flynn futebolista (Eslováquia)", "Voetbal-Flynn (Slowakije)", "サッカーフリン（スロバキア）", "축구 플린 (슬로바키아)",
     "Jalkapallo-Flynn (Slovakia)", "Fotball-Flynn (Slovakia)", "Fotbolls-Flynn (Slovakien)", "足球費恩（斯洛伐克）",
     "Флинн-футболист (Словакия)", "Flynn piłkarz (Słowacja)", nullptr, "Flynn do futebol (Eslováquia)",
     "Flynn ποδοσφαιριστής (Σλοβακία)", "Flynn fotbalista (Slovensko)", "Futbolcu Flynn (Slovakya)"}},
    {0xd6802d9cu, {"Soccer Flynn (Spain)", "Flynn footballeur (Espagne)", "Flynn calciatore (Spagna)",
     "Fußball-Flynn (Spanien)", "Flynn futbolista (España)", "Fodbold-Flynn (Spanien)",
     "Flynn futebolista (Espanha)", "Voetbal-Flynn (Spanje)", "サッカーフリン（スペイン）", "축구 플린 (스페인)",
     "Jalkapallo-Flynn (Espanja)", "Fotball-Flynn (Spania)", "Fotbolls-Flynn (Spanien)", "足球費恩（西班牙）",
     "Флинн-футболист (Испания)", "Flynn piłkarz (Hiszpania)", nullptr, "Flynn do futebol (Espanha)",
     "Flynn ποδοσφαιριστής (Ισπανία)", "Flynn fotbalista (Španělsko)", "Futbolcu Flynn (İspanya)"}},
    {0xcc617741u, {"Soccer Flynn (Uruguay)", "Flynn footballeur (Uruguay)", "Flynn calciatore (Uruguay)",
     "Fußball-Flynn (Uruguay)", "Flynn futbolista (Uruguay)", "Fodbold-Flynn (Uruguay)",
     "Flynn futebolista (Uruguai)", "Voetbal-Flynn (Uruguay)", "サッカーフリン（ウルグアイ）", "축구 플린 (우루과이)",
     "Jalkapallo-Flynn (Uruguay)", "Fotball-Flynn (Uruguay)", "Fotbolls-Flynn (Uruguay)", "足球費恩（烏拉圭）",
     "Флинн-футболист (Уругвай)", "Flynn piłkarz (Urugwaj)", nullptr, "Flynn do futebol (Uruguai)",
     "Flynn ποδοσφαιριστής (Ουρουγουάη)", "Flynn fotbalista (Uruguay)", "Futbolcu Flynn (Uruguay)"}},
    {0x416d1b31u, {"Soccer Flynn (USA)", "Flynn footballeur (États-Unis)", "Flynn calciatore (USA)",
     "Fußball-Flynn (USA)", "Flynn futbolista (EE. UU.)", "Fodbold-Flynn (USA)", "Flynn futebolista (EUA)",
     "Voetbal-Flynn (VS)", "サッカーフリン（アメリカ）", "축구 플린 (미국)", "Jalkapallo-Flynn (Yhdysvallat)", "Fotball-Flynn (USA)",
     "Fotbolls-Flynn (USA)", "足球費恩（美國）", "Флинн-футболист (США)", "Flynn piłkarz (USA)", nullptr,
     "Flynn do futebol (EUA)", "Flynn ποδοσφαιριστής (ΗΠΑ)", "Flynn fotbalista (USA)", "Futbolcu Flynn (ABD)"}},
};

// Ids whose text is another id's.
const struct { uint32_t id, to; } kAliases[] = {
    {0x828d76dbu, 0xa30341e7u},   // net-patch9 Deathmatch playlist -> PS4 Deathmatch
    {0x7c60126eu, 0x3b8de400u},   // net-patch9 Elimination playlist -> PS4 Elimination
    {0xf0a21209u, 0xadbc5c93u},   // net-patch9 Siege game type -> PS4 Siege
    {0x7a89e967u, 0x307e3705u},   // net-patch9 message of the day -> PS4 welcome text
    {0x74aff594u, kTextAllCompetitive},   // net-patch9 playlist
    {0x9a13e8f3u, kTextAllCompetitiveDesc},   // net-patch9 playlist
    {0x893405b5u, kTextTheLab},   // net-patch9 playlist
    {0xbfddf64au, kTextTheLabDesc},   // net-patch9 playlist
    {0x34865555u, kTextEliminationDesc},   // net-patch9 playlist
    {0xc020db25u, 0x3f4b6279u},   // net-patch9 3v3 (second id)
};

// Greek, Czech and Turkish for the texts their PS4 banks have only as "_" (common: the multiplayer menu,
// networking: every multiplayer text), our own wording from the PS4 English. Used only when the game's
// text is "_" (czech has 3 common and 9 networking texts of its own). Terms follow the PS4 common banks
// of these languages: booster = ενίσχυση / vylepšení / eklenti, medal = μετάλλιο / medaile / madalya,
// Co-op = Συνεταιρικό / kooperativní / Eşli, Villains = Αντίπαλοι / Padouši / Kötüler.
struct MenuText {
    uint32_t id;
    const char *el, *cs, *tr;
};
const MenuText kMenu[] = {
    {0x8c850436u, "Αποδοχή", "Přijmout", "Kabul Et"},   // Accept
    {0x910fb4f9u, "Προσθήκη φίλου", "Přidat přítele", "Arkadaş Ekle"},   // Add Friend
    {0xe50f12bau, "Ενισχύσεις", "Vylepšení", "Eklentiler"},   // Boosters
    {0x2ef3c8b1u, "ΟΠΛΑ ΣΥΝΕΡΓΑΣΙΑΣ", "ZBRANĚ PRO KOOPERACI", "ORTAK OYUN SİLAHLARI"},   // CO-OP WEAPONS
    {0x81493ae7u, "Ακύρωση", "Zrušit", "İptal"},   // Cancel
    {0x61bf0714u, "Χαρακτήρας", "Postava", "Karakter"},   // Character
    {0x249f5194u, "Διάλεξε τις ενισχύσεις σου!", "Vyberte si vylepšení!", "Eklentilerini seç!"},   // Choose your boosters!
    {0xcb379354u, "Κινηματογράφος", "Biograf", "Sinema"},   // Cinema
    {0xb0ac49b5u, "Κατηγορία", "Třída", "Sınıf"},   // Class
    {0xad9f50d9u, "Σύνδεση...", "Připojování...", "Bağlanılıyor..."},   // Connecting...
    {0xa7304dd3u, "Δημιούργησε ένα παιχνίδι για τους φίλους σου", "Vytvořte hru pro své přátele",
     "Arkadaşların için bir oyun oluştur"},   // Create a game for your friends
    {0x03ef26e6u, "Δημιουργία ομάδας...", "Vytváření skupiny...", "Grup oluşturuluyor..."},   // Creating Party...
    {0x66d63005u, "Προσαρμοσμένο παιχνίδι", "Vlastní hra", "Özel Oyun"},   // Custom Game
    {0x4ab1bf29u, "Προσαρμοσμένο παιχνίδι", "Vlastní hra", "Özel Oyun"},   // Custom Game
    {0xdb87e858u, "Απόρριψη", "Odmítnout", "Reddet"},   // Decline
    {0x59f9b620u, "Δυσκολία", "Obtížnost", "Zorluk"},   // Difficulty
    {0x8c3be05au, "Εύρεση αγώνα", "Najít zápas", "Maç Bul"},   // Find Match
    {0x022b777du, "Βρες γρήγορα έναν αγώνα", "Rychle najít zápas", "Hızlı bir maç bul"},   // Find a quick match
    {0x8c0b0a5cu, "Συγχώρεση", "Odpustit", "Affet"},   // Forgive
    {0x6e83df53u, "Φίλοι", "Přátelé", "Arkadaşlar"},   // Friends
    {0x131c09bbu, "Τύπος παιχνιδιού", "Typ hry", "Oyun Türü"},   // Game Type
    {0x47bb9c98u, "Το παιχνίδι ξεκινά σε [A]...", "Hra začne za [A]...", "Oyun [A] içinde başlıyor..."},   // Game starting in [A]...
    {0x2de07d54u, "Γεια σου. Θέλεις να γίνουμε φίλοι;", "Ahoj. Budeme přátelé?", "Merhaba. Arkadaş olalım mı?"},   // Hello. Be my friend?
    {0xaffe606fu, "Ήρωας", "Hrdina", "Kahraman"},   // Hero
    {0x30c2375bu, "Φιλοξενία παιχνιδιού", "Hostovat hru", "Oyun Kur"},   // Host Game
    {0xdcc66f88u, "Πρόσκληση φίλων στην ομάδα", "Pozvat přátele do skupiny", "Arkadaşları Gruba Davet Et"},   // Invite Friends to Party
    {0x4a555973u, "Πρόσκληση στην ομάδα", "Pozvat do skupiny", "Gruba Davet Et"},   // Invite To Party
    {0x885ef7e9u, "Προσκλήσεις", "Pozvánky", "Davetler"},   // Invites
    {0xd5a8546au, "Συμμετοχή σε παιχνίδι (Debug)", "Připojit se ke hře (ladění)", "Oyuna Katıl (Hata Ayıklama)"},   // Join Game (Debug)
    {0xb1600ce3u, "Συμμετοχή στην ομάδα", "Připojit se ke skupině", "Gruba Katıl"},   // Join Party
    {0xd5969d29u, "Συμμετοχή στο παιχνίδι...", "Připojování ke hře...", "Oyuna katılınıyor..."},   // Joining Game...
    {0xe4e03363u, "Συμμετοχή στην ομάδα...", "Připojování ke skupině...", "Gruba katılınıyor..."},   // Joining Party...
    {0x10e5f8ecu, "Αποβολή", "Vyhodit", "At"},   // Kick
    {0x1e2955afu, "Αποβολή από την ομάδα", "Vyhodit ze skupiny", "Gruptan At"},   // Kick From Party
    {0x6318c53fu, "Αποχώρηση", "Odejít", "Ayrıl"},   // Leave
    {0x9d4f678bu, "Αποχώρηση από το παιχνίδι", "Opustit hru", "Oyundan Ayrıl"},   // Leave Game
    {0x4bef8dc4u, "Αποχώρηση από το παιχνίδι και απώλεια των κερδών σου;", "Opustit hru a přijít o výdělek?",
     "Oyundan ayrılıp kazançlarından vazgeçmek istiyor musun?"},   // Leave the game and abandon your earnings?
    {0x2df029e8u, "Αποχώρηση από το παιχνίδι;", "Opustit hru?", "Oyundan ayrılmak istiyor musun?"},   // Leave the game?
    {0xbe329231u, "Αποχώρηση από το παιχνίδι...", "Opouštění hry...", "Oyundan ayrılınıyor..."},   // Leaving Game...
    {0xf14a7bf8u, "Επίπεδο", "Úroveň", "Seviye"},   // Level
    {0x19491ce1u, "ΠΟΛΛΟΙ ΠΑΙΚΤΕΣ", "HRA VÍCE HRÁČŮ", "ÇOK OYUNCULU"},   // MULTIPLAYER
    {0x32d0f1f5u, "Χάρτης", "Mapa", "Harita"},   // Map
    {0xbd1d2c53u, "Εύρεση αντιπάλων", "Vytváření hry", "Eşleştirme"},   // Matchmaking
    {0xf75b3a65u, "Πολλοί παίκτες", "Hra více hráčů", "Çok Oyunculu"},   // Multiplayer
    {0x37d314bbu, "Πολλοί παίκτες", "Hra více hráčů", "Çok Oyunculu"},   // Multiplayer
    {0xffbbac12u, "Σίγαση", "Ztlumit", "Sessize Al"},   // Mute
    {0xd381d5d8u, "Σίγαση όλων", "Ztlumit vše", "Tümünü Sessize Al"},   // Mute All
    {0x6ea711d6u, "Εκτός σύνδεσης", "Offline", "Çevrimdışı"},   // Offline
    {0x51663192u, "Σε σύνδεση", "Online", "Çevrimiçi"},   // Online
    {0x093f8561u, "Άλλες ρυθμίσεις", "Další nastavení", "Diğer Ayarlar"},   // Other Settings
    {0xb6bd34c3u, "Ομάδα", "Skupina", "Grup"},   // Party
    {0xac182308u, "Αρχηγός ομάδας", "Vůdce skupiny", "Grup Lideri"},   // Party Leader
    {0x4771c0fcu, "Μέλη ομάδας", "Členové skupiny", "Grup Üyeleri"},   // Party Members
    {0x51257d8bu, "Ομάδα", "Skupina", "Grup Kur"},   // Party Up
    {0xeb758483u, "Λειτουργία παιχνιδιού", "Herní režim", "Oyun Modu"},   // Play Mode
    {0xacb77201u, "Παίκτες που γνώρισες", "Potkaní hráči", "Karşılaşılan Oyuncular"},   // Players Met
    {0xb529ca4eu, "Λίστα αγώνων", "Seznam her", "Oyun Listesi"},   // Playlist
    {0xb102e089u, "Προφίλ", "Profil", "Profil"},   // Profile
    {0x9fdf8887u, "Ορισμός ως αρχηγού ομάδας", "Povýšit na vůdce skupiny", "Grup Lideri Yap"},   // Promote To Party Leader
    {0xa787b1adu, "Τύπος κατάταξης", "Typ hodnocení", "Sıralama Türü"},   // Rank Type
    {0xad4cdb32u, "Με κατάταξη", "Hodnocená", "Dereceli"},   // Ranked
    {0x633a9d67u, "Επανάληψη τελευταίου παιχνιδιού", "Přehrát poslední hru", "Son Oyunu Tekrar Oynat"},   // Replay Last Game
    {0xf221549du, "Αποτελέσματα", "Výsledky", "Sonuçlar"},   // Results
    {0xa9ac1ac2u, "Αποτελέσματα", "Výsledky", "Sonuçlar"},   // Results
    {0x7f532e93u, "Όριο γύρων", "Limit kol", "Tur Sınırı"},   // Round Limit
    {0xee4a67bfu, "Αποθήκευση τελευταίου παιχνιδιού", "Uložit poslední hru", "Son Oyunu Kaydet"},   // Save Last Game
    {0x86da827fu, "Όριο πόντων", "Limit skóre", "Skor Sınırı"},   // Score Limit
    {0xb3911b0du, "Αναζήτηση παικτών...", "Hledání hráčů...", "Oyuncu aranıyor..."},   // Searching for players...
    {0xec92411au, "Ορισμός ομάδας: Αυτόματα", "Nastavit tým: Automaticky", "Takım Seç: Otomatik"},   // Set Team: Auto
    {0xb1b5b7a4u, "Ορισμός ομάδας: Ήρωες", "Nastavit tým: Hrdinové", "Takım Seç: Kahramanlar"},   // Set Team: Heroes
    {0x4a2dbbdau, "Ορισμός ομάδας: Αντίπαλοι", "Nastavit tým: Padouši", "Takım Seç: Kötüler"},   // Set Team: Villains
    {0x75d524d8u, "Ρυθμίσεις", "Nastavení", "Ayarlar"},   // Settings
    {0x094c9a82u, "Θέση 1", "Pozice 1", "Yuva 1"},   // Slot 1
    {0x0d8d8735u, "Θέση 2", "Pozice 2", "Yuva 2"},   // Slot 2
    {0x7c8c4672u, "Κοινωνικό (χωρίς κατάταξη)", "Společenská (nehodnocená)", "Sosyal (Derecesiz)"},   // Social (Not Ranked)
    {0xf1de63c9u, "Έναρξη", "Začít", "Başlat"},   // Start
    {0x02237f2fu, "Έναρξη κινηματογράφου", "Spustit biograf", "Sinemayı Başlat"},   // Start Cinema
    {0x92db2b96u, "Έναρξη παιχνιδιού", "Začít hru", "Oyunu Başlat"},   // Start Game
    {0x69deab50u, "Έναρξη παιχνιδιού...", "Spouštění hry...", "Oyun başlatılıyor..."},   // Starting Game...
    {0x41aaf999u, "Συγχρονισμός...", "Synchronizace...", "Eşitleniyor..."},   // Syncing...
    {0x013c469au, "ΕΠΙΛΟΓΕΣ ΟΜΑΔΑΣ", "MOŽNOSTI TÝMU", "TAKIM SEÇENEKLERİ"},   // TEAM OPTIONS
    {0xae3ef529u, "Η ομάδα στην οποία προσπάθησες να μπεις δεν υπάρχει πια.",
     "Skupina, ke které jste se chtěli připojit, už neexistuje.", "Katılmaya çalıştığın grup artık yok."},   // The party you tried to join doesn't exist anymore.
    {0xfa9c4838u, "Κατάργηση σίγασης", "Zrušit ztlumení", "Sesi Aç"},   // Unmute
    {0x3efad3bau, "Κατάργηση σίγασης όλων", "Zrušit ztlumení všech", "Tümünün Sesini Aç"},   // Unmute All
    {0xfcac898du, "Παραλλαγή", "Varianta", "Varyasyon"},   // Variation
    {0xa332b573u, "Βέτο", "Veto", "Veto"},   // Veto
    {0x4040b706u, "Προβολή προφίλ", "Zobrazit profil", "Profili Görüntüle"},   // View Profile
    {0x659991a9u, "Αντίπαλος", "Padouch", "Kötü"},   // Villain
    {0x81cc5e93u, "Ψήφος", "Hlasovat", "Oy Ver"},   // Vote
    {0x747a57c3u, "Η ψηφοφορία λήγει σε [A]...", "Hlasování skončí za [A]...", "Oylama [A] içinde bitiyor..."},   // Voting ends in [A]...
    {0x63091cd0u, "Αναμονή για τον αρχηγό της ομάδας...", "Čekání na vůdce skupiny...", "Grup lideri bekleniyor..."},   // Waiting for Party Leader...
    {0xcd6b0ca7u, "Παρακολούθηση τελευταίου παιχνιδιού", "Sledovat poslední hru", "Son Oyunu İzle"},   // Watch Last Game
    {0x675dc197u, "Παρακολούθηση αποθηκευμένου παιχνιδιού", "Sledovat uloženou hru", "Kayıtlı Oyunu İzle"},   // Watch Saved Game
    {0x81801ffcu, "Παρακολούθησε ένα καταγεγραμμένο παιχνίδι", "Sledovat nahranou hru", "Kaydedilmiş bir oyunu izle"},   // Watch a recorded game
    {0xe8cf79e7u, "Ο συμπαίκτης σου, [TEXT], σε σκότωσε πολλές φορές.", "Váš spoluhráč [TEXT] vás několikrát zabil.",
     "Takım arkadaşın [TEXT] seni birçok kez öldürdü."},   // Your teammate, [TEXT], has killed you multiple times.
    {0xec840854u, "[A] παίκτες σε σύνδεση", "Hráči online: [A]", "[A] oyuncu çevrimiçi"},   // [A] players online
    // ---- networking bank
    {0x58023afcu, "Έναν παίκτη που τον πνίγουν πρέπει να τον σώσουν οι άλλοι παίκτες.",
     "Hráče, kterého někdo škrtí, musí zachránit ostatní hráči.",
     "Boğulan bir oyuncuyu diğer oyuncular kurtarmalıdır."},   // A player being choked must be rescued by the other_players.
    {0x682e6699u, "Η σκόπευση είναι πάντα πιο εύστοχη από το να τρέχεις και να πυροβολείς.",
     "Míření je vždy přesnější než střelba za běhu.",
     "Nişan almak, koşarken ateş etmekten her zaman daha isabetlidir."},   // Aiming is always more accurate than running and_gunning.
    {0x3f6bbbddu, "Πρέπει να είναι παρόντα όλα τα μέλη της ομάδας", "Musí být přítomni všichni členové skupiny",
     "Tüm grup üyeleri hazır olmalı"},   // All party members must be present
    {0xfc8dd900u, "Ένας συνδυασμός τύπων Αγώνα Θανάτου.", "Mix her typu Deathmatch.",
     "Çeşitli Ölüm Maçı oyun türleri."},   // An assortment of Deathmatch game types.
    {0x675ef552u, "Ένας συνδυασμός παιχνιδιών με αποστολές.", "Mix her s úkoly.",
     "Çeşitli görev tabanlı oyun türleri."},   // An assortment of Objective based game types.
    {0xa48d8b9au, "Σίγουρα θέλεις να το διαγράψεις;", "Opravdu chcete odstranit?", "Silmek istediğine emin misin?"},   // Are you sure you want to delete?
    {0x52f3ac2bu, "Επίθεση", "Útok", "Saldırı"},   // Assault
    {0x293ac7e7u, "Βοήθειες", "Asistence", "Asistler"},   // Assists
    {0x30c29caeu, "Παράσημο Τιμής: Μισή υγεία", "Čestný odznak: Poloviční zdraví", "Onur Nişanı: Yarım Sağlık"},   // Badge of Honor: Half Health
    {0x02d1b22fu, "Παράσημο Τιμής: Λιγότερες σφαίρες σε κάθε γεμιστήρα", "Čestný odznak: Méně nábojů v zásobníku",
     "Onur Nişanı: Her şarjörde daha az mermi"},   // Badge of Honor: Less bullets in each clip
    {0x82f94e80u, "Παράσημο Τιμής: Οι κοντινοί εχθροί βλέπουν το όνομά σου μέσα από τους τοίχους",
     "Čestný odznak: Blízcí nepřátelé vidí vaše jméno skrz zdi",
     "Onur Nişanı: Yakındaki düşmanlar adını duvarların ardından görür"},   // Badge of Honor: Nearby enemies see your name through walls
    {0xff392194u, "Παράσημο Τιμής: Χωρίς στόχαστρο στα τυφλά πυρά",
     "Čestný odznak: Bez zaměřovače při střelbě naslepo", "Onur Nişanı: Körlemesine ateşte nişangâh yok"},   // Badge of Honor: No Blindfire Reticle
    {0x3cc23bd2u, "Παράσημο Τιμής: Διπλή ζημιά από επιθέσεις σώμα με σώμα",
     "Čestný odznak: Dvojnásobné zranění od útoků zblízka", "Onur Nişanı: Yakın dövüş saldırılarından iki kat hasar"},   // Badge of Honor: Take double damage from hand-to-hand attacks
    {0xb73e0c82u, "Φυσιγγιοθήκη", "Bandalír", "Fişeklik"},   // Bandoleer
    {0x11073e7eu, "Ανταγωνιστική λίστα beta", "Soutěžní seznam her beta", "Beta Rekabetçi Oyun Listesi"},   // Beta Competitive Playlist
    {0xd3519643u, "Συνεταιρική λίστα beta", "Kooperativní seznam her beta", "Beta Eşli Oyun Listesi"},   // Beta Cooperative Playlist
    {0x595cd97eu, "Καλύτερη ευστοχία στη σκόπευση με τυφέκια", "Vyšší přesnost při míření dlouhými zbraněmi",
     "Uzun namlulu silahlarla nişan alırken daha iyi isabet"},   // Better accuracy when aiming long guns
    {0x9ce505a1u, "Καλύτερη ευστοχία στη σκόπευση με πιστόλια", "Vyšší přesnost při míření pistolemi",
     "Tabancalarla nişan alırken daha iyi isabet"},   // Better accuracy when aiming pistols
    {0xf15f8a73u, "Χωρισμός", "Rozchod", "Ayrılık"},   // Break Up
    {0x32cc1acbu, "Φέρε τον θησαυρό πίσω στη βάση σου", "Doneste poklad na svou základnu",
     "Hazineyi üssüne geri getir"},   // Bring the treasure back to your base
    {0x43b07fd4u, "Μετάφερε τον θησαυρό σου απέναντι στον χάρτη.", "Doneste svůj poklad přes celou mapu.",
     "Hazineni haritanın öbür ucuna taşı."},   // Bring your treasure accross the map.
    {0x9528faecu, "Ο εικονολήπτης Jeff", "Kameraman Jeff", "Kameraman Jeff"},   // Cameraman Jeff
    {0xe6087158u, "Κατάλαβε και κράτησε τις ζώνες", "Obsaďte a udržte zóny", "Bölgeleri ele geçir ve tut"},   // Capture and hold the zones
    {0x953f7955u, "Κατάλαβε την περιοχή", "Obsaďte území", "Bölgeyi Ele Geçir"},   // Capture the Territory
    {0x0c9287e5u, "Κατάλαβε τον θησαυρό.", "Získejte poklad.", "Hazineyi ele geçir."},   // Capture the treasure.
    {0x4d6c148au, "Κατάλαβε τις ζώνες με τη σειρά.", "Obsaďte zóny v pořadí.", "Bölgeleri sırayla ele geçir."},   // Capture the zones in order.
    {0xf6618b0cu, "Καταλήψεις", "Obsazení", "Ele Geçirmeler"},   // Captures
    {0xed900acdu, "Μεταφέρεις μια επιπλέον χειροβομβίδα", "Jeden granát navíc", "Fazladan bir el bombası taşı"},   // Carry an extra grenade
    {0x81bd1955u, "Μεταφέρεις περισσότερα πυρομαχικά", "Více střeliva", "Daha fazla cephane taşı"},   // Carry more ammo
    {0xf625a024u, "Αλυσιδωτή Αντίδραση", "Řetězová reakce", "Zincirleme Tepki"},   // Chain Reaction
    {0x71ede2dcu, "Αλυσιδωτή Αντίδραση", "Řetězová reakce", "Zincirleme Tepki"},   // Chain Reaction
    {0x4162699bu, "Δες αυτό το αρχείο κινηματογράφου του Uncharted 2!",
     "Podívejte se na tento soubor biografu z Uncharted 2!", "Bu Uncharted 2 Sinema dosyasına bir bak!"},   // Check out this Uncharted 2 Cinema file!
    {0x8561d1c8u, "Σημείο ελέγχου", "Kontrolní bod", "Kontrol Noktası"},   // Checkpoint
    {0xe028d51bu, "Σημείο ελέγχου", "Kontrolní bod", "Kontrol Noktası"},   // Checkpoint
    {0x98a8b0a7u, "Σημεία ελέγχου", "Kontrolní body", "Kontrol Noktaları"},   // Checkpoints
    {0x21e7724eu,
     "Τα σημεία ελέγχου επαναφέρουν στη ζωή όλους τους πεσμένους συμπαίκτες. Βέβαια, καλύτερα να μην τους αφήσεις να πέσουν από την αρχή.",
     "Kontrolní body oživí všechny padlé spoluhráče. Samozřejmě jste je ale vůbec neměli nechat padnout.",
     "Kontrol noktaları düşen tüm takım arkadaşlarını hayata döndürür. Tabii en başta düşmelerine izin vermemeliydin."},   // Checkpoints bring back all fallen teammates back to life.  O
    {0x3dd23662u, "Chloe Frazer", "Chloe Frazer", "Chloe Frazer"},   // Chloe Frazier
    {0x51800192u, "Όσους πνίγουν μπορείς να τους χτυπήσεις μόνο από πίσω.", "Škrtiče lze udeřit jen zezadu.",
     "Boğan düşmanlara yalnızca arkadan yumruk atılabilir."},   // Chokers can only be punched from_behind.
    {0x1a587883u, "Αρχείο κινηματογράφου", "Soubor biografu", "Sinema Dosyası"},   // Cinema File
    {0xdb64ce56u, "Σκαρφαλώνεις γρηγορότερα", "Rychlejší šplhání", "Daha hızlı tırman"},   // Climb faster
    {0x1dbaf8b3u, "Κοντινή Μάχη", "Boj zblízka", "Yakın Mesafe"},   // Close Quarters
    {0x2ee192abu, "Συνεταιρική Αρένα", "Kooperativní aréna", "Eşli Arena"},   // Co-op Arena
    {0x42f2ce6fu, "Συνεταιρική δυσκολία", "Kooperativní obtížnost", "Eşli Oyun Zorluğu"},   // Co-op Difficulty
    {0x5136a578u, "Συνεταιρική Αποστολή", "Kooperativní úkol", "Eşli Görev"},   // Co-op Objective
    {0xaaf8fb47u, "Αναβάθμιση συνεταιρικού όπλου", "Vylepšení kooperativní zbraně", "Eşli Oyun Silah Geliştirmesi"},   // Co-op Weapon Upgrade
    {0x25a005abu, "Μολών Λαβέ", "Pojď si pro to", "Gel Biraz Al"},   // Come Get Some
    {0x7eb5a5c4u, "Περιέχει παιχνίδια Κατάληψης Σημαίας και Αγώνα Θανάτου",
     "Obsahuje hry typu Ukradni vlajku i Deathmatch", "Hem Bayrak Kapmaca hem Ölüm Maçı türü oyunlar içerir"},   // Contains Both CTF and Deathmatch Style Games
    {0xf49e6c49u, "Αδυσώπητο", "Drtivá", "Ezici"},   // Crushing
    {0xa0345923u, "ΗΤΤΑ", "PORÁŽKA", "YENİLGİ"},   // DEFEAT
    {0x21fe8a6eu, "Νεκρός Εξερευνητής", "Mrtvý průzkumník", "Ölü Kâşif"},   // Dead Explorer
    {0xa30341e7u, "Αγώνας Θανάτου", "Deathmatch", "Ölüm Maçı"},   // Deathmatch
    {0x8dfdede9u, "Αγώνας Θανάτου", "Deathmatch", "Ölüm Maçı"},   // Deathmatch
    {0x0bfa3d45u, "Αγώνες Θανάτου με παραλλαγές στα όπλα και στους κανόνες",
     "Hry typu Deathmatch s obměnami zbraní a pravidel", "Silah ve kural değişiklikleri olan Ölüm Maçı oyunları"},   // Deathmatch Games with Variations to Weapons and Rules
    {0x57fc82aau, "Αγώνες Θανάτου και παιχνίδια με αποστολές. Καταγράφεται το επίπεδο δεξιότητας.",
     "Hry typu Deathmatch a hry s úkoly. Zaznamenává se úroveň dovednosti.",
     "Ölüm Maçı ve görev türü oyunlar. Beceri seviyesi kaydedilir."},   // Deathmatch and Objective game types. Skill Level tracked.
    {0x1ca241b4u, "Λειτουργίες Αγώνα Θανάτου και Λεηλασίας", "Herní režimy Deathmatch a Rabování",
     "Ölüm Maçı ve Yağma Oyun Modları"},   // Deathmatch and Plunder Game Modes
    {0xcd3394e9u, "Θάνατοι", "Smrti", "Ölümler"},   // Deaths
    {0xad203afeu, "Χάρτης αποσφαλμάτωσης", "Ladicí mapa", "Hata Ayıklama Haritası"},   // Debug Map
    {0x44edfab7u, "Δοκιμαστικός χάρτης αποσφαλμάτωσης", "Ladicí testovací mapa", "Hata Ayıklama Test Haritası"},   // Debug Test Map
    {0xaf2201fau, "Λίστα αποσφαλμάτωσης μόνο για δοκιμές", "Ladicí seznam her jen pro testování",
     "Yalnızca test için hata ayıklama listesi"},   // Debug playlist for testing only
    {0x395bc3e7u, "Μείωση ζημιάς από εκρήξεις", "Menší zranění od výbuchů", "Patlamalardan Alınan Hasarı Azalt"},   // Decrease Damage from Explosions
    {0x4935b394u, "Μείωση χρόνου κατάληψης και διατήρησης περιοχών", "Kratší doba obsazení a držení území",
     "Bölge Ele Geçirme ve Tutma Sürelerini Azalt"},   // Decrease Territory Capture and Hold Times
    {0x8fda1e6du, "Νίκησε τον αρχηγό", "Porazte vůdce", "Elebaşını hakla"},   // Defeat the ringleader
    {0xc2335ee0u, "Νίκησε τους στρατιώτες για να εξασφαλίσεις τον θησαυρό.", "Porazte vojáky a zajistěte poklad.",
     "Hazineyi güvenceye almak için askerleri hakla."},   // Defeat the soldiers to secure the treasure.
    {0x4b064c20u,
     "Κάθε όπλο δίνει διαφορετικούς πόντους όταν νικάς εχθρούς. Πειραματίσου για τα καλύτερα αποτελέσματα.",
     "Za poražení nepřátel různými zbraněmi získáte různý počet bodů. Experimentujte pro nejlepší výsledky.",
     "Düşmanları farklı silahlarla haklamak farklı puanlar kazandırır. En iyi sonuç için dene."},   // Defeating enemies with different guns results in different p
    {0xd1fbd1e3u, "Υπεράσπισε τον θησαυρό", "Braňte poklad", "Hazineyi savun"},   // Defend the treasure
    {0xcbdc2a22u, "Κατάθεση", "Uložit", "Teslim Et"},   // Deposit
    {0xefa450a3u, "Βάθος πεδίου", "Hloubka ostrosti", "Alan Derinliği"},   // Depth of Field
    {0x3eec4b3fu, "Αποκορεσμός", "Odbarvení", "Doygunluk Azaltma"},   // Desaturation
    {0x7788ec2au,
     "Το ήξερες; Στο πρώτο μέρος κάθε συνεταιρικού επιπέδου υπάρχει ένα πιστόλι Desert-5. Προσπάθησε να το βρεις!",
     "Víte, že v první části každé kooperativní úrovně je pistole Desert-5? Zkuste ji najít!",
     "Biliyor muydun? Her eşli bölümün ilk kısmında bir Desert-5 tabanca var. Bulmaya çalış!"},   // Did you know? There is a Desert-5 pistol in the first part o
    {0x61ac5df7u, "Το ήξερες; Για να μαζέψεις πυρομαχικά γρηγορότερα, κάνε κυβίστηση πάνω τους και πάτησε |T|.",
     "Víte, že střelivo seberete rychleji, když se přes něj překulíte a stisknete |T|?",
     "Biliyor muydun? Cephaneyi daha hızlı almak için üzerinden yuvarlan ve |T| tuşuna bas."},   // Did you know? To pick up ammo faster, roll over it and press
    {0x28654920u, "Το ήξερες; Μπορείς να αγοράσεις αναβαθμίσεις όπλων για το Συνεταιρικό στο Κατάστημα!",
     "Víte, že vylepšení zbraní pro kooperaci lze koupit v obchodě?",
     "Biliyor muydun? Eşli oyun için silah geliştirmelerini Mağaza'dan alabilirsin!"},   // Did you know? Weapon upgrades can be purchased for Co-op in 
    {0xd8b32bc0u, "Σκάψιμο", "Kopání", "Kazı"},   // Dig
    {0xc767dd31u, "Απόσταση από το θέμα", "Vzdálenost od objektu", "Nesneye Uzaklık"},   // Distance to Subject
    {0x444ab84du, "Διπλά χρήματα μέχρι το Επίπεδο 10", "Dvojnásobek peněz do úrovně 10",
     "10. Seviyeye kadar İki Kat Para"},   // Double Money until Level 10
    {0x22ca3cf2u, "Drake με Ντόνατ", "Kobliha Drake", "Çörekçi Drake"},   // Doughnut Drake
    {0x9572fc51u, "Κάτω τα Όπλα", "Zbraně dolů", "Silahları İndir"},   // Down the Irons
    {0x6c51a743u, "Πεσμένος", "Sražen", "Yere Düştü"},   // Downed
    {0xf1e0390bu, "Λήψη στα τοπικά αρχεία...", "Stahování do místních souborů...", "Yerel Dosyalara İndiriliyor..."},   // Downloading to Local Files...
    {0x7039261fu, "Dragan", "Dragan", "Dragan"},   // Dragan
    {0x398edb0cu, "Drake", "Drake", "Drake"},   // Drake
    {0x1e9aa6aeu, "Αφήνεις ενεργή χειροβομβίδα όταν πεθαίνεις", "Po smrti upustíte odjištěný granát",
     "Ölünce pimi çekilmiş bir el bombası bırak"},   // Drop a live grenade upon death
    {0xda32c701u, "Περισσότερα χρήματα από μετάλλια αποστολών", "Více peněz za medaile za úkoly",
     "Görev madalyalarından daha fazla para kazan"},   // Earn more money for objective medals
    {0x5754a7e8u, "Elena Fisher", "Elena Fisher", "Elena Fisher"},   // Elena Fisher
    {0xbb8baf42u, "Εξόντωσε την άλλη ομάδα.", "Zlikvidujte druhý tým.", "Diğer takımı yok et."},   // Eliminate the other team.
    {0x3b8de400u, "Εξόντωση", "Eliminace", "Eleme"},   // Elimination
    {0x1220854eu, "Αποφυγή", "Úhyb", "Kaçınma"},   // Evasion
    {0x415aeaa7u, "Ειδικός στα Εκρηκτικά", "Expert na výbušniny", "Patlayıcı Uzmanı"},   // Explosive Expert
    {0xb07f4a22u, "ΑΠΕΤΥΧΕ", "NEÚSPĚCH", "BAŞARISIZ"},   // FAILED
    {0x4268b9e5u, "Δοκιμή παρατάξεων", "Test frakcí", "Hizip Testi"},   // Factions Test
    {0x2c053d10u, "Γρήγορη κίνηση εμπρός", "Přetočit vpřed", "İleri Sar"},   // Fast Forward
    {0x0a1eb8beu, "Γρηγορότερη αναπλήρωση υγείας", "Rychlejší obnova zdraví", "Daha Hızlı Sağlık Yenilenmesi"},   // Faster Health Regeneration
    {0xc6f2064eu, "Γρηγορότερη επαναφόρτωση τυφεκίων", "Rychlejší přebíjení dlouhých zbraní",
     "Uzun namlulu silahları daha hızlı doldur"},   // Faster long-gun reloading
    {0x4f667313u, "Γρηγορότερη επαναφόρτωση πιστολιών", "Rychlejší přebíjení pistolí", "Tabancaları daha hızlı doldur"},   // Faster pistol reloading
    {0x185afbe2u, "Πολέμησε για τον θησαυρό.", "Bojujte o poklad.", "Hazine için savaş."},   // Fight for the Treasure.
    {0xc0759450u, "Εύρεση παίκτη...", "Hledání hráče...", "Oyuncu Aranıyor..."},   // Finding Player...
    {0xca175b38u, "Γροθοπατινάδα", "Pěstní souboj", "Yumruk Yumruğa"},   // Fisticuffs
    {0x837848fau, "Γοργοπόδαρος", "Lehká noha", "Çevik Ayak"},   // Fleet Foot
    {0xb666a543u, "Φιγούρα", "Svaly", "Kas Gösterisi"},   // Flex
    {0x151363e3u, "Καταιγισμός", "Smršť", "Yaylım"},   // Flurry
    {0x1924d576u, "Πρόκληση «Καταιγισμός»", "Provokace Smršť", "Yaylım alayı"},   // Flurry taunt
    {0x956b09d8u, "Εστιακό εύρος", "Ohniskový rozsah", "Odak Aralığı"},   // Focal Range
    {0x3b613ffcu,
     "Για την εύρεση αντιπάλων στο Συνεταιρικό, μπορείς να επιλέξεις την προτίμηση δυσκολίας στο Προφίλ σου.",
     "Pro vytváření kooperativních her si můžete zvolit obtížnost ve svém profilu.",
     "Eşli oyun eşleştirmesi için zorluk tercihini Profil'inden seçebilirsin."},   // For Co-op Matchmaking, you can select your preference for di
    {0xd08e0327u, "Φρούριο", "Pevnost", "Kale"},   // Fort
    {0xa91680a8u, "Βρέθηκε παίκτης", "Hráč nalezen", "Oyuncu Bulundu"},   // Found Player
    {0x9b7aa1a9u, "Φίλια πυρά", "Palba do vlastních", "Dost Ateşi"},   // Friendly Fire
    {0x25266ff9u, "Τα φίλια πυρά είναι ενεργά στο Δύσκολο και στο Αδυσώπητο. Καλή τύχη, θα τη χρειαστείς.",
     "Na těžkou a drtivou obtížnost je zapnutá palba do vlastních. Hodně štěstí, budete ho potřebovat.",
     "Dost ateşi Zor ve Ezici zorlukta açıktır. Bol şans, ihtiyacın olacak."},   // Friendly fire is active on Hard and Crushing difficulty.  Go
    {0x97450a1au, "Από το Ισχίο", "Od boku", "Kalçadan"},   // From The Hip
    {0xd38a7d18u, "ΣΤΟΧΟΣ", "CÍL", "HEDEF"},   // GOAL
    {0x819b0348u, "Τζένγκις Χαν", "Čingischán", "Cengiz Han"},   // Genghis Khan
    {0x31a791f5u, "Περισσότερα πυρομαχικά από κάθε συλλογή", "Více střeliva z každého sebrání",
     "Her alışta daha fazla cephane al"},   // Get more ammo from each pickup
    {0x87fa8aeeu, "Βγες από το στενό", "Dostaňte se z uličky", "Sokaktan çık"},   // Get out of the alley 
    {0x93de85dcu, "Πάρε τα εισιτήριά σου για το σόου των όπλων", "Lístky na přehlídku zbraní",
     "Silah gösterisine biletini al"},   // Get your tickets to the gun show
    {0xa23ee558u, "Γυάλινο Σαγόνι", "Skleněná čelist", "Cam Çene"},   // Glass Jaw
    {0x61c14068u, "Πήγαινε να διεκδικήσεις τη χρυσή σου λεία", "Jděte si pro svou zlatou kořist",
     "Git ve altın ganimetini al"},   // Go and claim your bounty of gold
    {0x00248bb5u, "Πυρετός του Χρυσού", "Zlatá horečka", "Altına Hücum"},   // Gold Rush
    {0xe9995f4cu, "Πυρετός του Χρυσού", "Zlatá horečka", "Altına Hücum"},   // Gold Rush
    {0xb8a9d6edu, "Χρυσά Όπλα", "Zlaté zbraně", "Altın Silahlar"},   // Golden Guns
    {0x73baf91au, "Βαρύτητα", "Gravitace", "Yerçekimi"},   // Gravity
    {0x50dee705u, "Πράσινη οθόνη", "Zelené plátno", "Yeşil Perde"},   // Green Screen
    {0xd64d6a88u, "Μισογεμάτος", "Poloviční zásobník", "Yarım Dolu"},   // Half Loaded
    {0xcabc423fu, "Harry Flynn", "Harry Flynn", "Harry Flynn"},   // Harry Flynn
    {0x94c99eeeu, "Κατευθύνσου προς την κόκκινη, καλοφτιαγμένη πόρτα", "Jděte k červeným, pěkně zdobeným dveřím",
     "Kırmızı ve özenle işlenmiş kapıya doğru git"},   // Head towards the red, and well textured, door
    {0x0abdc1eeu, "Κατευθύνσου προς την αγορά", "Jděte k tržišti", "Pazara doğru git"},   // Heads towards the market
    {0x99e71e1bu, "Βολές στο κεφάλι", "Zásahy do hlavy", "Kafadan Vuruşlar"},   // Headshots
    {0x0b8e5a64u, "Drake διαρρήκτης", "Lupič Drake", "Soyguncu Drake"},   // Heist Drake
    {0xcf9558c0u, "Flynn διαρρήκτης", "Lupič Flynn", "Soyguncu Flynn"},   // Heist Flynn
    {0x965c5c6eu, "Πύρινη Κόλαση", "Pekelný oheň", "Cehennem Ateşi"},   // Hell Blazer
    {0x9ba36948u, "Βοηθήστε ο ένας τον άλλον να φτάσετε στην άλλη πλευρά των δρόμων",
     "Pomozte si navzájem dostat se na druhou stranu ulic",
     "Birbirinize sokakların öbür tarafına ulaşmak için yardım edin"},   // Help each reach the other side of the streets
    {0x4fc75197u,
     "Μετά τον θάνατό σου βοήθησε τους συμπαίκτες σου λέγοντας πού είναι οι εχθροί. Χρησιμοποίησε το |D-PAD| για να αλλάξεις κάμερες.",
     "Po smrti pomáhejte spoluhráčům hlášením pozic nepřátel. Pomocí |D-PAD| přepínáte kamery.",
     "Öldükten sonra düşman konumlarını bildirerek takım arkadaşlarına yardım et. Kameralar arasında geçiş için |D-PAD| kullan."},   // Help your teammates after death by calling out enemy positio
    {0x856e4bb2u, "Έρχονται...", "Už jdou...", "İşte geliyorlar..."},   // Here they come...
    {0x6a022a4bu, "Ήρωες", "Hrdinové", "Kahramanlar"},   // Heroes
    {0x26ec10ceu, "Νίκησαν οι Ήρωες!", "Vyhráli hrdinové!", "Kahramanlar Kazandı!"},   // Heroes Win!
    {0x8f16bbc2u, "Κράτησε όσες περισσότερες ζώνες μπορείς.", "Udržte co nejvíce zón.",
     "Tutabildiğin kadar çok bölge tut."},   // Hold as many zones as you can.
    {0x9c45e621u, "Κράτησε τον λόφο.", "Udržte kopec.", "Tepeyi tut."},   // Hold the hill.
    {0xf8b79f98u,
     "Κράτα πατημένο το |L1| για σκόπευση και πάτησε το |L2| για να ρίξεις χειροβομβίδα εκεί που δείχνει το στόχαστρο.",
     "Podržením |L1| míříte a klepnutím na |L2| hodíte granát tam, kam ukazuje zaměřovač.",
     "Nişan almak için |L1| tuşunu basılı tut, nişangâhın gösterdiği yere el bombası atmak için |L2| tuşuna dokun."},   // Hold |L1| to aim and tap |L2| to throw a grenade where your 
    {0xfc76822fu,
     "Κράτα πατημένο το |L2| για να εμφανιστεί ένα τόξο που δείχνει πού ακριβώς θα πέσει η χειροβομβίδα.",
     "Podržením |L2| zobrazíte oblouk, který ukazuje, kam přesně granát dopadne.",
     "El bombasının tam olarak nereye düşeceğini gösteren yayı görmek için |L2| tuşunu basılı tut."},   // Hold |L2| to display an arc that shows exactly where your gr
    {0xe231d22au, "Κράτα πατημένο το |L2| για να δεις πού θα πεταχτεί το αντικείμενο που κρατάς.",
     "Podržením |L2| uvidíte, kam neseným předmětem hodíte.",
     "Taşıdığın eşyanın nereye atılacağını görmek için |L2| tuşunu basılı tut."},   // Hold |L2| to see where a carried item will be_thrown.
    {0x9c96a51au, "Όταν κρατάς θησαυρό, κράτα πατημένο το |L2| για να δεις πού θα πεταχτεί.",
     "Když nesete poklad, podržením |L2| uvidíte, kam ho hodíte.",
     "Hazine taşırken nereye atılacağını görmek için |L2| tuşunu basılı tut."},   // Hold |L2| when carrying a Treasure to see where it will be_t
    {0x5cc3274bu, "Κράτα πατημένο το |T| για να αναστήσεις έναν πεσμένο συνεργάτη.",
     "Podržením |T| oživíte sraženého parťáka.", "Yere düşen bir ortağı canlandırmak için |T| tuşunu basılı tut."},   // Hold |T| to revive a downed_partner.
    {0xac610c4bu, "Ο οικοδεσπότης αποχώρησε", "Hostitel odešel", "Kurucu Ayrıldı"},   // Host Left
    {0x4284774eu, "Αν όλη η ομάδα σου αποτύχει 3 φορές, το παιχνίδι τελειώνει.",
     "Pokud celý váš tým třikrát selže, hra končí.", "Bütün takımın 3 kez başarısız olursa oyun biter."},   // If your entire team fails 3 times, the game is_over.
    {0xc749762du, "Ανοσία στην Επίγνωση Κατάστασης", "Odolnost vůči Přehledu o situaci",
     "Durum Farkındalığına Bağışıklık"},   // Immunity to Situational Awareness
    {0x84041d79u, "Στην αίθουσα αναμονής", "V předsálí", "Lobide"},   // In lobby
    {0xac91c910u, "Αύξηση ζημιάς χειροβομβίδων", "Větší zranění granáty", "El Bombası Hasarını Artır"},   // Increase Grenade Damage
    {0xa85c17f8u, "Αύξηση ακτίνας έκρηξης χειροβομβίδων", "Větší dosah výbuchu granátů",
     "El Bombası Patlama Alanını Artır"},   // Increase Grenade Explosion Radius
    {0xbb4568b9u, "Αύξηση απόστασης ρίψης χειροβομβίδων", "Delší hod granátem", "El Bombası Atış Mesafesini Artır"},   // Increase Grenade Throw Distance
    {0xcbed2252u, "Αύξηση ευστοχίας τυφεκίων στα τυφλά πυρά", "Vyšší přesnost dlouhých zbraní při střelbě naslepo",
     "Uzun Namlulu Silahlarda Körlemesine Ateş İsabetini Artır"},   // Increase Long Gun Blindfire Accuracy
    {0x3c231dc4u, "Αύξηση μέγιστης υγείας", "Vyšší maximální zdraví", "Azami Sağlığı Artır"},   // Increase Max Health
    {0x0a482f2cu, "Αύξηση ευστοχίας πιστολιών στα τυφλά πυρά", "Vyšší přesnost pistolí při střelbě naslepo",
     "Tabancalarda Körlemesine Ateş İsabetini Artır"},   // Increase Pistol Blindfire Accuracy
    {0x773ec9c8u, "Αύξηση ζημιάς πιστολιών", "Větší zranění pistolemi", "Tabanca Hasarını Artır"},   // Increase Pistol Damage
    {0x4471443au, "Αύξηση ζημιάς τυφεκίων", "Větší zranění puškami", "Tüfek Hasarını Artır"},   // Increase Rifle Damage
    {0x4e067c1fu, "Αύξηση ζημιάς τυφεκίων ελεύθερου σκοπευτή", "Větší zranění odstřelovacími puškami",
     "Keskin Nişancı Tüfeği Hasarını Artır"},   // Increase Sniper Rifle Damage
    {0xd87fe73bu, "Μεγαλύτερη χωρητικότητα πυρομαχικών για το M32-Hammer", "Větší kapacita střeliva M32-Hammer",
     "M32-Hammer için daha fazla cephane kapasitesi"},   // Increased M32-Hammer ammo capacity
    {0x085bf21fu, "Μεγαλύτερη χωρητικότητα πυρομαχικών για το RPG_-_7", "Větší kapacita střeliva RPG_-_7",
     "RPG_-_7 için daha fazla cephane kapasitesi"},   // Increased RPG_-_7 ammo capacity
    {0x1baa6a2eu, "Μεγαλύτερη ζημιά σε αντικείμενα που σπάνε", "Větší poškození rozbitných předmětů",
     "Kırılabilir nesnelere daha fazla hasar"},   // Increased damage to breakable objects
    {0x9a223ee3u, "inFAMOUS - Cole", "inFAMOUS - Cole", "inFAMOUS - Cole"},   // Infamous - Cole
    {0x8f3bc4cbu, "inFAMOUS - Κακός Cole", "inFAMOUS - Zlý Cole", "inFAMOUS - Kötü Cole"},   // Infamous - Cole Evil
    {0xbedb2632u, "Αναπηρία", "Invalida", "Hastalıklı"},   // Invalid
    {0xe2069b80u, "Έλα στην ομάδα μου στο Uncharted 2 Πολλοί παίκτες!",
     "Přidejte se k mé skupině ve hře více hráčů Uncharted 2!", "Uncharted 2 Çok Oyunculu grubuma katıl!"},   // Join my Uncharted 2 Multiplayer Party!
    {0xaf4c6db2u, "Ταχυδακτυλουργός", "Žonglér", "Hokkabaz"},   // Juggler
    {0xad40639du, "Karl Schäfer", "Karl Schäfer", "Karl Schäfer"},   // Karl Schäfer
    {0x16d028c6u, "Συνέχισε να Πυροβολείς", "Nepřestávej pálit", "Ateşe Devam"},   // Keep Firing
    {0x49f5e231u, "Εξοντώσεις", "Zabití", "Leşler"},   // Kills
    {0x13f76fccu, "Βασιλιάς του Λόφου", "Král kopce", "Tepenin Kralı"},   // King of the Hill
    {0x37621ab4u, "Φιλί", "Polibek", "Öpücük"},   // Kiss
    {0x12da3a2bu, "Φιλί του Θανάτου", "Polibek smrti", "Ölüm Öpücüğü"},   // Kiss of Death
    {0x0a7e5b3au, "Τελευταία προσπάθεια", "Poslední pokus", "Son Deneme"},   // Last Try
    {0xe94ef6ecu, "Γέλιο", "Smích", "Kahkaha"},   // Laugh
    {0x2cb274b2u, "Εκτοξευτής", "Odpalovač", "Fırlatıcı"},   // Launch Man
    {0x800a7a54u, "Υπολοχαγός Draza", "Poručík Draza", "Teğmen Draza"},   // Lieutenant Draza
    {0x0a7f93d8u, "Μεγάλη Απόσταση", "Dlouhý dosah", "Uzun Menzil"},   // Long Range
    {0x0f915f15u, "Lovac", "Lovac", "Lovac"},   // Lovac
    {0x5614c769u, "Machinima", "Machinima", "Machinima"},   // Machinima
    {0xe289e135u, "Λειτουργία Machinima", "Režim Machinima", "Machinima modu"},   // Machinima mode
    {0x9799b4bcu, "Πεπρωμένο", "Zjevný osud", "Kader"},   // Manifest Destiny
    {0xc4dede6bu, "Δοκιμή εστίασης Μαρτίου", "Březnový test", "Mart Odak Testi"},   // March Focus Test
    {0x1c288c37u, "Μάρκο Πόλο", "Marco Polo", "Marco Polo"},   // Marco Polo
    {0x93ac6446u, "Ο αγώνας τελειώνει σε [TIME]", "Zápas skončí za [TIME]", "Maç [TIME] içinde bitiyor"},   // Match ends in [TIME]
    {0x558f6c67u, "Έφτασες τον μέγιστο αριθμό διαδικτυακών αρχείων!", "Dosažen maximální počet on-line souborů!",
     "Azami Çevrimiçi Dosya sayısına ulaşıldı!"},   // Maximum number of Online Files reached!
    {0xa3b60175u, "Μετάλλιο", "Medaile", "Madalya"},   // Medal
    {0x3fb98809u, "Μετάλλια", "Medaile", "Madalyalar"},   // Medals
    {0x1842529cu, "Μηνύματα", "Zprávy", "Mesajlar"},   // Messages
    {0xf99ecaecu, "Μεταφορά οικοδεσπότη...", "Přenášení hostitele...", "Kurucu Taşınıyor..."},   // Migrating Host...
    {0x3b377961u, "Η αποστολή ολοκληρώθηκε σε [CLOCK]", "Mise dokončena za [CLOCK]", "Görev [CLOCK] içinde tamamlandı"},   // Mission Completed in [CLOCK]
    {0x6327a51au, "Η αποστολή απέτυχε στον γύρο [A]", "Mise selhala v kole [A]", "Görev [A]. turda başarısız oldu"},   // Mission Failed at Round [A]
    {0x169c9ab4u, "Η αποστολή απέτυχε!", "Mise selhala!", "Görev Başarısız!"},   // Mission Failed!
    {0x2b43bf87u, "Μοναστήρι", "Klášter", "Manastır"},   // Monastary
    {0xf2a350bcu, "Άνθρωπος Μαϊμού", "Opičák", "Maymun Adam"},   // Monkey Man
    {0x669ec424u, "Περισσότερη XP για ολοκληρωμένες αποστολές", "Více XP za splněné úkoly",
     "Tamamlanan görevler için daha fazla XP"},   // More XP for completed objectives
    {0x5a4d01bdu, "Περισσότερες σφαίρες σε κάθε γεμιστήρα", "Více nábojů v zásobníku", "Her şarjörde daha fazla mermi"},   // More bullets in each clip
    {0xbab7700bu, "Κινείσαι γρηγορότερα όταν σκοπεύεις", "Rychlejší pohyb při míření",
     "Nişan alırken daha hızlı hareket et"},   // Move faster while aiming
    {0x0b9188b3u, "Κινείσαι γρηγορότερα με το GAU_-_19", "Rychlejší pohyb s GAU_-_19",
     "GAU_-_19 ile daha hızlı hareket et"},   // Move faster with the GAU_-_19
    {0xf2b5a7dau, "Κινείσαι γρηγορότερα με την ασπίδα", "Rychlejší pohyb se štítem",
     "Çevik kuvvet kalkanıyla daha hızlı hareket et"},   // Move faster with the Riot Shield
    {0x1725c461u, "Κινείσαι γρηγορότερα με τον θησαυρό", "Rychlejší pohyb s pokladem",
     "Hazineyle daha hızlı hareket et"},   // Move faster with the Treasure
    {0x6df770eeu, "Πολλαπλασιαστής", "Násobič", "Çarpan"},   // Multiplier
    {0x9b7568c5u, "ND Drake", "ND Drake", "ND Drake"},   // ND Drake
    {0x8246cbf7u, "Πειρατής ND", "Pirát ND", "ND Korsanı"},   // ND Pirate
    {0x8ca7d6edu, "Υπάλληλος της Naughty Dog", "Zaměstnanec Naughty Dog", "Naughty Dog Çalışanı"},   // Naughty Dog Employee
    {0x35d85e1fu, "Εμπόλεμη ζώνη του Νεπάλ", "Válečná zóna v Nepálu", "Nepal Savaş Bölgesi"},   // Nepal Warzone
    {0x9ddb8c8fu, "Επίπεδο δοκιμής δικτύου", "Úroveň pro test sítě", "Ağ Test Bölümü"},   // Network Test Level
    {0xfa1c0a7au, "Νέος λόφος", "Nový kopec", "Yeni Tepe"},   // New Hill
    {0x81da48e3u, "Ξεκλειδώθηκε νέα δυσκολία!", "Odemknuta nová obtížnost!", "Yeni zorluk açıldı!"},   // New difficulty unlocked!
    {0x0516aef4u, "Χωρίς επανεμφάνιση", "Bez oživení", "Yeniden Doğma Yok"},   // No Respawn
    {0x0a301d33u, "Χωρίς χρονικό μπόνους", "Bez časového bonusu", "Zaman Bonusu Yok"},   // No Time Bonus
    {0x37e7c290u, "Δεν πέφτεις από τις εκρήξεις", "Výbuchy vás nesrazí", "Patlamalar seni yere düşürmez"},   // No knockdown from explosions
    {0xed22a97du, "Χωρίς κίνηση πρόκλησης", "Bez animace provokace", "Alay animasyonu yok"},   // No taunt animation
    {0xc89a89e2u, "Κανένα", "Žádné", "Yok"},   // None
    {0xc63da515u, "Κανένα", "Žádné", "Yok"},   // None
    {0x53f6e665u, "Δεν έχει εξοπλιστεί τίποτα", "Nic není vybaveno", "Hiçbir Şey Kuşanılmadı"},   // Nothing Equipped
    {0x8f86cb5fu, "Αποστολή", "Úkol", "Görev"},   // Objective
    {0x970feb2cu, "Συνεταιρική λειτουργία αποστολών", "Kooperativní režim s úkoly", "Eşli Görev Modu"},   // Objective Cooperative Mode
    {0x4bf30e73u, "Αποστολές", "Úkoly", "Görevler"},   // Objectives
    {0x4bdabbd1u, "Τίμιος", "Na úrovni", "Dürüst"},   // On the Level
    {0x82c61354u, "Πρόσκληση σε ομάδα", "Pozvánka do skupiny", "Grup Daveti"},   // Party Invite
    {0x2c6bd0c7u, "Παύση", "Pozastavit", "Duraklat"},   // Pause
    {0xe6f0cf9du, "Σήκωσε φιάλες προπανίου με το |T| και πέτα τες με το |L2|.",
     "Propanové lahve seberete pomocí |T| a hodíte pomocí |L2|.", "Propan tüplerini |T| ile al ve |L2| ile at."},   // Pick up propane tanks with |T| and throw them with_|L2|.
    {0x91d583c3u, "Σήκωσε τον θησαυρό με το |T| και πέτα τον με το |L2|.",
     "Poklad seberete pomocí |T| a hodíte pomocí |L2|.", "Hazineyi |T| ile al ve |L2| ile at."},   // Pick up the Treasure with |T| and throw it with_|L2|.
    {0x7349f19du, "Μάζεψε τους θησαυρούς που λάμπουν για να αυξήσεις τον μετρητή συνδυασμών.",
     "Sbírejte třpytivé poklady a zvyšte si ukazatel komba.",
     "Kombo göstergeni artırmak için parlayan hazineleri topla."},   // Pick up the sparkling treasures to increase your combo_meter
    {0xfbb2d6a3u, "Πιστόλια και χειροβομβίδες", "Pistole a granáty", "Tabancalar ve El Bombaları"},   // Pistols & Grenades
    {0x26e4add9u, "Αναπαραγωγή", "Přehrát", "Oynat"},   // Play
    {0x77f62775u, "Παίζει παιχνίδι ενός παίκτη", "Hraje hru pro jednoho hráče", "Tek Oyunculu Oynuyor"},   // Playing Single-Player
    {0x953825d0u, "Παίζει άλλο παιχνίδι", "Hraje jinou hru", "Başka bir oyun oynuyor"},   // Playing another game
    {0xcf292b57u, "Παίζει αγώνα", "Hraje zápas", "Maç oynuyor"},   // Playing match
    {0x3d9470a5u, "Λεηλασία", "Rabování", "Yağma"},   // Plunder
    {0x1645995du, "Σημάδεψε και Πυροβόλησε", "Zamiř a střílej", "Nişan Al ve Vur"},   // Point and Shoot
    {0x3c666c2eu, "Πόντοι", "Body", "Puan"},   // Points
    {0xf2083786u, "Πόντοι", "Body", "Puan"},   // Points
    {0x3f4a4219u, "Τρόμπα", "Pumpa", "Pompa"},   // Pump
    {0x1af26286u, "Δώσ' του", "Pumpuj", "Gaza Bas"},   // Pump it up
    {0x2594bd65u, "Γρήγορος Γεμιστήρας", "Rychlý zásobník", "Hızlı Şarjör"},   // Quick Clip
    {0x9f107953u, "Γρήγορα Χέρια", "Rychlé ruce", "Hızlı Eller"},   // Quick Hands
    {0xe1167dfau, "ΓΥΡΟΣ", "KOLO", "TUR"},   // ROUND
    {0x392f8928u, "RPG", "RPG", "RPG'ler"},   // RPG's
    {0x47b24464u, "Με κατάταξη", "Hodnocená", "Dereceli"},   // Ranked
    {0x0146986bu, "Ταχύτατα Χέρια", "Bleskové ruce", "Seri Eller"},   // Rapid Hands
    {0xb88fc6a6u, "Λιγότερο τρέμουλο από πόνο όταν σκοπεύεις με διόπτρα",
     "Menší otřes při zranění během míření optikou", "Dürbünle bakarken acıdan daha az sarsılma"},   // Reduced pain wobble while scoped in
    {0x2f709c13u, "Χαλαρή κίνηση", "Uvolněný pohyb", "Rahat Hareket"},   // Relaxed Movement
    {0xebb3e944u, "Θυμήσου να γεμίζεις με το |R2| όποτε έχεις την ευκαιρία.",
     "Nezapomeňte při každé příležitosti přebít pomocí |R2|.", "Her fırsatta |R2| ile şarjör değiştirmeyi unutma."},   // Remember to reload using |R2| every chance you_get.
    {0x6cef7b2eu,
     "Θυμήσου να σκέφτεσαι και κάθετα. Όλα τα επίπεδα έχουν πολλά σημεία για να σκαρφαλώσεις και να κρυφτείς ψηλά.",
     "Myslete i vertikálně. Všechny úrovně mají mnoho míst, kam lze vyšplhat a schovat se nad zemí.",
     "Dikey düşünmeyi unutma. Tüm bölümlerde tırmanıp yerden yüksekte saklanacak birçok yer var."},   // Remember to think vertically. All levels have many places to
    {0xa251dfd5u, "Επανεμφάνιση σε [A]...", "Oživení za [A]...", "[A] içinde yeniden doğacaksın..."},   // Respawn in [A]...
    {0x61f6f20du, "Επανεμφάνιση", "Oživování", "Yeniden Doğuyor"},   // Respawning
    {0x11d63f40u, "Εκδίκηση", "Pomsta", "İntikam"},   // Revenge
    {0x2951e0f7u, "Πρόωρη πρόσβαση στην ενίσχυση «Εκδίκηση»", "Předčasný přístup k vylepšení Pomsta",
     "İntikam Eklentisine Erken Erişim"},   // Revenge Booster Early Access
    {0x5f0bcd65u, "Αναστάσεις", "Oživení", "Canlandırmalar"},   // Revives
    {0x3fa0224bu, "Όταν αναστήνεις έναν πεσμένο συμπαίκτη, γεμίζει η υγεία σου. Βγες εκεί έξω και σώσ' τους!",
     "Oživení sraženého spoluhráče vám doplní zdraví. Jděte a zachraňte je!",
     "Yere düşen bir takım arkadaşını canlandırmak sağlığını doldurur. Çık ve onları kurtar!"},   // Reviving a teammate from a downed state refills your health.
    {0xa39eb442u, "Επιστροφή στην αρχή", "Přetočit na začátek", "Başa Sar"},   // Rewind to Beginning
    {0xa43921abu, "XP τυφεκιοφόρου", "XP střelce", "Tüfekçi XP"},   // Rifleman XP
    {0x8c7e1d6du, "Ο Άνθρωπος Ρουκέτα", "Raketový muž", "Roket Adam"},   // Rocket Man
    {0x584327dfu, "Γύρος", "Kolo", "Tur"},   // Round
    {0x2aff2f55u, "Γύρος [A]", "Kolo [A]", "[A]. Tur"},   // Round [A]
    {0xeea064c4u, "Ο γύρος [A] ολοκληρώθηκε", "Kolo [A] dokončeno", "[A]. Tur Tamamlandı"},   // Round [A] Complete
    {0xfe71bcecu, "Γύρος [A]: Νίκησαν οι Ήρωες", "Kolo [A]: Vyhráli hrdinové", "[A]. Tur: Kahramanlar Kazandı"},   // Round [A]: Heroes Win
    {0xe2c9d42cu, "Γύρος [A]: Ισοπαλία", "Kolo [A]: Remíza", "[A]. Tur: Berabere"},   // Round [A]: Tie
    {0xf9875feau, "Γύρος [A]: Νίκησαν οι Αντίπαλοι", "Kolo [A]: Vyhráli padouši", "[A]. Tur: Kötüler Kazandı"},   // Round [A]: Villains Win
    {0x686c1461u, "Γύροι", "Kola", "Turlar"},   // Rounds
    {0xd043ee47u, "Ερείπια", "Ruiny", "Harabeler"},   // Ruins
    {0x02d279ccu, "Sark", "Sark", "Sark"},   // Sark
    {0xd47103d2u, "Ρακοσυλλέκτης", "Mrchožrout", "Leş Toplayıcı"},   // Scavenger
    {0xde88b467u, "Χαιρεκακία", "Škodolibost", "Başkasının Acısı"},   // Schadenfreude
    {0x48ecac62u, "Με διόπτρα", "Zaměřeno optikou", "Dürbünde"},   // Scoped In
    {0xe50f5f44u, "Βαθμολογία", "Skóre", "Skor"},   // Score
    {0xeb6201abu, "Αναζήτηση οποιουδήποτε παιχνιδιού", "Hledání libovolné hry", "Herhangi Bir Oyun Aranıyor"},   // Searching for Any Game
    {0xefa31c1cu, "Αναζήτηση κοντινού παιχνιδιού", "Hledání blízké hry", "Yakın Oyun Aranıyor"},   // Searching for Close Game
    {0xe2e03ac5u, "Αναζήτηση ιδανικού παιχνιδιού", "Hledání optimální hry", "En Uygun Oyun Aranıyor"},   // Searching for Optimal Game
    {0x1307a11fu, "Επιλέξιμη στολή για πολλούς παίκτες", "Volitelný vzhled pro hru více hráčů",
     "Seçilebilir çok oyunculu görünüm"},   // Selectable multiplayer skin
    {0xee98504cu, "Πυροβόλα τους εχθρούς σου στο πρόσωπο.", "Střílejte nepřátele do obličeje.",
     "Düşmanlarını yüzünden vur."},   // Shoot your enemies in the face.
    {0xe3f66a73u, "Αφεντικό με Καραμπίνα", "Boss s brokovnicí", "Pompalı Patron"},   // Shotgun Boss
    {0xafad7c9au, "XP καραμπίνας", "XP brokovnice", "Pompalı XP"},   // Shotgunner XP
    {0x92b2cdc8u, "Αθόρυβη κίνηση", "Tichý pohyb", "Sessiz hareket"},   // Silent movement
    {0x35b56be9u, "Επίγνωση Κατάστασης", "Přehled o situaci", "Durum Farkındalığı"},   // Situational Awareness
    {0x0c69b8bbu, "Skelzor", "Skelzor", "Skelzor"},   // Skelzor
    {0x15834349u, "Αργά", "Pomalu", "Yavaş"},   // Slow
    {0x1c3b8e33u, "Τυφέκιο ελεύθερου σκοπευτή και Pistole", "Odstřelovací puška a Pistole",
     "Keskin Nişancı Tüfeği ve Pistole"},   // Sniper Rifle & Pistole
    {0xeb4fc45cu, "Τυφέκια ελεύθερου σκοπευτή και Pistole", "Odstřelovací pušky a Pistole",
     "Keskin Nişancı Tüfekleri ve Pistole'lar"},   // Sniper Rifles & Pistole's
    {0xbb9145e2u, "XP ελεύθερου σκοπευτή", "XP odstřelovače", "Keskin Nişancı XP"},   // Sniper XP
    {0x6a805befu, "Σκηνή", "Scéna", "Sahne"},   // Stage
    {0x22a276ecu, "Κανονικό", "Standardní", "Standart"},   // Standard
    {0x62b38e06u, "Κλέψε τον θησαυρό από τους στρατιώτες.", "Ukradněte vojákům poklad.", "Hazineyi askerlerden çal."},   // Steal the treasure from the soldiers.
    {0x0ccf39d0u, "Κρυφή κίνηση", "Plíživý pohyb", "Gizli Hareket"},   // Stealth Movement
    {0x9125b4f3u, "Βήμα εμπρός", "Krok vpřed", "Bir Adım İleri"},   // Step Forward
    {0x981b5d11u, "Αυτοκτονίες", "Sebevraždy", "İntiharlar"},   // Suicides
    {0x511c120eu, "Σταθερό Βήμα", "Jistý krok", "Sağlam Adım"},   // Sure Foot
    {0x72f80f1bu, "Σίγουρη Βολή", "Jistá rána", "Tam İsabet"},   // Sure Shot
    {0xb81a7c18u, "Επιβίωση", "Přežití", "Hayatta Kalma"},   // Survival
    {0x2959eca4u, "Επιβίωση", "Přežití", "Hayatta Kalma"},   // Survival
    {0xe1ae2796u, "Επιβίωσε από την επίθεση!", "Přežijte nápor!", "Saldırıdan sağ kurtul!"},   // Survive the onslaught!
    {0xbf990995u, "Επιβίωσε από τα κύματα στρατιωτών.", "Přežijte vlny vojáků.", "Asker dalgalarından sağ kurtul."},   // Survive the waves of soldiers.
    {0x656749edu, "Επιβίωσε μαζί με τους φίλους σου.", "Přežijte se svými přáteli.", "Arkadaşlarınla hayatta kal."},   // Survive with your friends.
    {0x4db8b2d3u, "Λήψη στιγμιότυπου", "Pořídit snímek obrazovky", "Ekran Görüntüsü Al"},   // Take a Screenshot
    {0xd73d494eu, "Πάτησε το |T| ενώ σκοπεύεις για να αλλάξεις όπλο.",
     "Klepnutím na |T| během míření přepnete zbraň.", "Silah değiştirmek için nişan alırken |T| tuşuna dokun."},   // Tap |T| while aiming to switch_weapons.
    {0x7028a1ecu, "Πρόκληση", "Provokace", "Alay"},   // Taunt
    {0xdc2bdac5u, "Ομάδα", "Tým", "Takım"},   // Team
    {0x8eef517fu, "Εξοντώσεις συμπαικτών", "Zabití spoluhráčů", "Takım Arkadaşı Öldürme"},   // Team Kills
    {0x277cea5cu, "XP ομάδας", "XP týmu", "Takım XP"},   // Team XP
    {0x46456af9u, "Η ομαδική δουλειά είναι ο καλύτερος δρόμος προς την επιτυχία.",
     "Týmová spolupráce je nejlepší cestou k úspěchu.", "Takım çalışması başarının en iyi yoludur."},   // Teamwork is the best way to_succeed.
    {0x85353c9eu, "Νευρική κίνηση", "Napjatý pohyb", "Gergin Hareket"},   // Tense Movement
    {0x02329021u, "Tenzin", "Tenzin", "Tenzin"},   // Tenzin
    {0xc05bd881u, "Περιοχή", "Území", "Bölge"},   // Territory
    {0x66628784u, "Περιοχή", "Území", "Bölge"},   // Territory
    {0x7bedc4b2u, "Δοκιμή", "Test", "Test"},   // Test
    {0x7d7a00c6u, "Tetram", "Tetram", "Tetram"},   // Tetram
    {0x1dcca0f1u, "Η Παγωμένη Σπηλιά", "Ledová jeskyně", "Buz Mağarası"},   // The Ice Cave
    {0x9ddf8b90u, "Η Χαμένη Πόλη", "Ztracené město", "Kayıp Şehir"},   // The Lost City
    {0x6a1d2af2u, "Οι Θεοί των Πολλών Παικτών κατακεραύνωσαν τον", "Bohové hry více hráčů ztrestali hráče",
     "Çok Oyunculu Tanrıları çarptı:"},   // The Multiplayer Gods smote
    {0x23480697u, "Η Πλατεία", "Náměstí", "Meydan"},   // The Plaza
    {0xb5b2acd0u, "Το Άδυτο", "Svatyně", "Sığınak"},   // The Sanctuary
    {0x45206309u, "Η Σκηνή", "Scéna", "Sahne"},   // The Stage
    {0x8f4a3a42u, "Ο Ναός", "Chrám", "Tapınak"},   // The Temple
    {0x3af91b54u, "Τα Συντρίμμια του Τρένου", "Zničený vlak", "Tren Enkazı"},   // The Train Wreck
    {0xa036040eu, "Το Χωριό", "Vesnice", "Köy"},   // The Village
    {0x82d1f51fu, "Το σεντούκι κατάληψης δεν είναι πάντα στο έδαφος.", "Truhla pro obsazení není vždy na zemi.",
     "Ele geçirme sandığı her zaman yerde değildir."},   // The capture chest is not always at ground_level.
    {0x32e30492u, "Ο μετρητής συνδυασμών αυξάνεται σε κάθε επίπεδο δυσκολίας.",
     "Ukazatel komba roste s každou úrovní obtížnosti.", "Kombo göstergesi her zorluk seviyesinde artar."},   // The combo meter increases with each level of_difficulty. 
    {0x8610e8a8u, "Αν πετάς τον θησαυρό, τον μετακινείς στον χάρτη γρηγορότερα απ' ό,τι αν τον κουβαλάς.",
     "Házením se poklad dostane přes mapu rychleji než nošením.",
     "Hazineyi atmak, onu haritada taşımaktan daha hızlı ilerletir."},   // Throwing the Treasure can move it across the map faster than
    {0x5516c27fu, "Ισοπαλία!", "Remíza!", "Berabere!"},   // Tie Game!
    {0x451f9742u, "Χρονικό μπόνους!    Πολλαπλασιαστής πόντων x[A]", "Časový bonus!    Násobič bodů x[A]",
     "Zaman Bonusu!    x[A] Puan Çarpanı"},   // Time Bonus!    x[A] Point Multiplier
    {0x07774a0du, "Χρονικό όριο", "Časový limit", "Süre Sınırı"},   // Time Limit
    {0xf135a4f6u, "Για να ρίξεις χειροβομβίδα στα τυφλά, πάτησε το |L2| προς την κατεύθυνση της κάμερας.",
     "Granát naslepo hodíte klepnutím na |L2| ve směru kamery.",
     "Körlemesine el bombası atmak için kameranın baktığı yönde |L2| tuşuna dokun."},   // To blindfire a grenade, tap |L2| in the direction the camera
    {0xcaa6cd51u, "Σύνολο", "Celkem", "Toplam"},   // Total
    {0x70fa1751u, "Σύνολο", "Celkem", "Toplam"},   // Total
    {0x6b62220cu, "Φορέας Θησαυρού", "Nosič pokladu", "Hazine Taşıyıcı"},   // Treasure Bearer
    {0xdd7c8e56u, "Απόσπαση Θησαυρού", "Vynesení pokladu", "Hazine Çıkarma"},   // Treasure Extraction
    {0x0544e6f6u, "Πόλεμος Περιοχών", "Válka o území", "Bölge Savaşı"},   // Turf War
    {0xa269aa23u, "Χελώνα", "Želva", "Kaplumbağa"},   // Turtle
    {0xd39f8dd7u, "Αναβάθμιση", "Vylepšení", "Geliştirme"},   // Upgrade
    {0xed42bfdbu, "Αναβάθμισε το [TEXT] με καλύτερη ευστοχία.", "Vylepšete [TEXT] o vyšší přesnost.",
     "[TEXT] silahını daha iyi isabetle geliştir."},   // Upgrade [TEXT] with better accuracy.
    {0xe983a26cu, "Αναβάθμισε το [TEXT] με γρηγορότερη επαναφόρτωση.", "Vylepšete [TEXT] o rychlejší přebíjení.",
     "[TEXT] silahını daha hızlı doldurmayla geliştir."},   // Upgrade [TEXT] with faster reloading.
    {0xe0019902u, "Αναβάθμισε το [TEXT] με μεγαλύτερη χωρητικότητα πυρομαχικών.",
     "Vylepšete [TEXT] o větší kapacitu střeliva.", "[TEXT] silahını daha fazla cephane kapasitesiyle geliştir."},   // Upgrade [TEXT] with more ammo capacity.
    {0x2edffcc5u, "Χρησιμοποίησε ένα RPG-7 για να ανατινάξεις την πόρτα", "Vyhoďte dveře do vzduchu pomocí RPG-7",
     "Kapıyı havaya uçurmak için bir RPG-7 kullan"},   // Use an RPG-7 to blow out the door
    {0xa7567efau, "Χρησιμοποίησε ένα RPG-7 για να γκρεμίσεις τον τοίχο", "Zničte zeď pomocí RPG-7",
     "Duvarı yıkmak için bir RPG-7 kullan"},   // Use an RPG-7 to take out the wall structure
    {0xeb793b5du, "Χρησιμοποίησε συντονισμένες κρυφές επιθέσεις για να προχωρήσεις",
     "Postupujte pomocí koordinovaných útoků ze zálohy", "İlerlemek için koordineli gizli saldırılar kullan"},   // Use coordinate stealth attacks to proceed 
    {0xd3fc54f9u, "Χρησιμοποίησε το |D-PAD-LR| για να αλλάξεις όπλο.", "Pomocí |D-PAD-LR| přepnete zbraň.",
     "Silah değiştirmek için |D-PAD-LR| kullan."},   // Use |D-PAD-LR| to switch_weapons.
    {0xef72f4f3u, "Όταν κουβαλάς φιάλη προπανίου, σκόπευσε με το |L1| και πυροβόλησε με το πιστόλι σου με το |R1|.",
     "Když nesete propanovou lahev, míříte pomocí |L1| a z pistole střílíte pomocí |R1|.",
     "Propan tüpü taşırken nişan almak için |L1|, tabancanla ateş etmek için |R1| kullan."},   // Use |L1| to aim and |R1| to fire your pistol weapon while ca
    {0x9857b8adu, "Όταν κουβαλάς τον θησαυρό, σκόπευσε με το |L1| και πυροβόλησε με το πιστόλι σου με το |R1|.",
     "Když nesete poklad, míříte pomocí |L1| a z pistole střílíte pomocí |R1|.",
     "Hazineyi taşırken nişan almak için |L1|, tabancanla ateş etmek için |R1| kullan."},   // Use |L1| to aim and |R1| to fire your pistol while carrying 
    {0x656d4040u, "Χρησιμοποίησε το |RA| για ζουμ με τα τυφέκια.", "Pomocí |RA| přibližujete u pušek.",
     "Tüfeklerde yakınlaştırmak için |RA| kullan."},   // Use |RA| to zoom rifles.
    {0x2ca45497u, "Η μάχη σώμα με σώμα με θωρακισμένους εχθρούς είναι επικίνδυνη τακτική. Πρόσεχε.",
     "Boj zblízka proti obrněným nepřátelům je nebezpečná taktika. Buďte opatrní.",
     "Zırhlı düşmanlara karşı yakın dövüş tehlikeli bir taktiktir. Dikkatli ol."},   // Using melee against armored enemies is a dangerous tactic.  
    {0x077cca82u, "Άστοχος", "Zahalený", "Üstü Kapalı"},   // Veiled
    {0x15484e0du, "Victor Sullivan", "Victor Sullivan", "Victor Sullivan"},   // Victor Sullivan
    {0x759ae9beu, "Κάμερα παρακολούθησης", "Sledovací kamera", "Takip Kamerası"},   // View Follow Camera
    {0x4471e9dcu, "Ελεύθερη κάμερα", "Volná kamera", "Serbest Kamera"},   // View Free Camera
    {0xdb83632bu, "Περιστροφική κάμερα", "Orbitální kamera", "Yörünge Kamerası"},   // View Orbit Camera
    {0xdf6b09c2u, "Πυρετός του Χρυσού στο Χωριό", "Zlatá horečka ve vesnici", "Köyde Altına Hücum"},   // Village Gold Rush
    {0x6ec337fcu, "Αντίπαλοι", "Padouši", "Kötüler"},   // Villains
    {0x222d0d79u, "Νίκησαν οι Αντίπαλοι!", "Vyhráli padouši!", "Kötüler Kazandı!"},   // Villains Win!
    {0x3f94b955u, "Οπτικές ρυθμίσεις", "Vizuální úpravy", "Görsel Ayarlar"},   // Visual Tweaks
    {0xd0cbb810u, "Vodnik", "Vodnik", "Vodnik"},   // Vodnik
    {0xff92a836u, "Κίνηση με βάδισμα", "Chůze", "Yürüme Hareketi"},   // Walk Movement
    {0x317a1e2du, "Ελαφρύ Βήμα", "Tichý krok", "Sessiz Adım"},   // Walk Softly
    {0xea3bb70eu, "Παρακολουθεί κινηματογράφο", "Sleduje biograf", "Sinema izliyor"},   // Watching a cinema
    {0xa81b8e3fu, "Το κύμα 1 ολοκληρώθηκε", "Vlna 1 dokončena", "1. Dalga Tamamlandı"},   // Wave 1 Complete
    {0xa558a8e6u, "Το κύμα 2 ολοκληρώθηκε", "Vlna 2 dokončena", "2. Dalga Tamamlandı"},   // Wave 2 Complete
    {0xa199b551u, "Το κύμα 3 ολοκληρώθηκε", "Vlna 3 dokončena", "3. Dalga Tamamlandı"},   // Wave 3 Complete
    {0xbfdee554u, "Το κύμα 4 ολοκληρώθηκε", "Vlna 4 dokončena", "4. Dalga Tamamlandı"},   // Wave 4 Complete
    {0xbb1ff8e3u, "Το κύμα 5 ολοκληρώθηκε", "Vlna 5 dokončena", "5. Dalga Tamamlandı"},   // Wave 5 Complete
    {0xb65cde3au, "Το κύμα 6 ολοκληρώθηκε", "Vlna 6 dokončena", "6. Dalga Tamamlandı"},   // Wave 6 Complete
    {0xb29dc38du, "Το κύμα 7 ολοκληρώθηκε", "Vlna 7 dokončena", "7. Dalga Tamamlandı"},   // Wave 7 Complete
    {0x8ad27e30u, "Το κύμα 8 ολοκληρώθηκε", "Vlna 8 dokončena", "8. Dalga Tamamlandı"},   // Wave 8 Complete
    {0x8e136387u, "Το κύμα 9 ολοκληρώθηκε", "Vlna 9 dokončena", "9. Dalga Tamamlandı"},   // Wave 9 Complete
    {0xc70beaa3u,
     "Καλώς ήρθες στο Uncharted 2: Among Thieves Πολλοί παίκτες!\\\\Ευχαριστούμε που παίζεις!\\\\Δες τα στατιστικά σου και τα τελευταία νέα στο www.naughtydog.com\\Διασκέδασε με τη beta και απόκτησε το πλήρες παιχνίδι για ακόμα περισσότερους χάρτες και λειτουργίες!\\\\Ευχαριστούμε και καλή διασκέδαση!",
     "Vítejte ve hře více hráčů Uncharted 2: Among Thieves!\\\\Děkujeme, že hrajete!\\\\Své statistiky a nejnovější zprávy najdete na www.naughtydog.com\\Užijte si betu a pořiďte si plnou hru s dalšími mapami a režimy!\\\\Děkujeme a bavte se!",
     "Uncharted 2: Among Thieves Çok Oyunculu'ya hoş geldin!\\\\Oynadığın için teşekkürler!\\\\İstatistiklerine ve en son haberlere www.naughtydog.com adresinden göz at\\Beta'nın tadını çıkar ve daha fazla harita ve mod için oyunun tamamını edin!\\\\Teşekkürler, iyi eğlenceler!"},   // Welcome to Uncharted 2: Among Thieves Multiplayer!  Thanks f
    {0x307e3705u,
     "Καλώς ήρθες στο Uncharted 2: Among Thieves Πολλοί παίκτες!\\\\Ευχαριστούμε που παίζεις!\\\\Δες τα στατιστικά σου και τα τελευταία νέα στο www.naughtydog.com\\Θα ενημερώνουμε το παιχνίδι τακτικά, οπότε έχε τον νου σου εδώ για νέα.\\\\Ευχαριστούμε και καλή διασκέδαση!",
     "Vítejte ve hře více hráčů Uncharted 2: Among Thieves!\\\\Děkujeme, že hrajete!\\\\Své statistiky a nejnovější zprávy najdete na www.naughtydog.com\\Hru budeme pravidelně aktualizovat, sledujte proto toto místo.\\\\Děkujeme a bavte se!",
     "Uncharted 2: Among Thieves Çok Oyunculu'ya hoş geldin!\\\\Oynadığın için teşekkürler!\\\\İstatistiklerine ve en son haberlere www.naughtydog.com adresinden göz at\\Oyunu düzenli olarak güncelleyeceğiz, yenilikler için burayı takip et.\\\\Teşekkürler, iyi eğlenceler!"},   // Welcome to Uncharted 2: Among Thieves Multiplayer!  Thanks f
    {0xf03b77d4u, "Όταν στέκεσαι ακίνητος, πάτησε το πάνω κουμπί για να βλέπεις τους εχθρούς μέσα από τους τοίχους",
     "Když stojíte, stiskem tlačítka nahoru uvidíte nepřátele skrz zdi",
     "Hareketsiz dururken düşmanları duvarların ardından görmek için yukarı tuşuna bas"},   // When standing still, press the up button to see enemies thro
    {0x886d2c42u, "Χειμερινή Chloe", "Zimní Chloe", "Kışlık Chloe"},   // Winter Chloe
    {0x65ab2ef0u, "Χειμερινός Drake", "Zimní Drake", "Kışlık Drake"},   // Winter Drake
    {0x7cfd4e4du, "Χειμερινή Elena", "Zimní Elena", "Kışlık Elena"},   // Winter Elena
    {0x158d9966u, "Χειμερινός Flynn", "Zimní Flynn", "Kışlık Flynn"},   // Winter Flynn
    {0xde9a9177u, "Εξαφάνισε τα εχθρικά κύματα", "Zlikvidujte nepřátelské vlny", "Düşman dalgalarını sil süpür"},   // Wipe the enemy waves
    {0x7dd166dfu, "Ναι", "Ano", "Evet"},   // Yes
    {0x44022562u, "Ναιιιι", "Jóóó", "Evettt"},   // Yessss
    {0x99f0d970u, "Νίκησες!", "Vyhráli jste!", "Kazandın!"},   // You Win!
    {0xc35e16e8u, "Εσύ και οι φίλοι σου ενάντια σε όλο τον κόσμο!", "Vy a vaši přátelé proti celému světu!",
     "Sen ve arkadaşların dünyaya karşı!"},   // You and your friends against the world!
    {0x1e21ccb1u, "Δεν έχεις ελεύθερες θέσεις.", "Nemáte žádnou volnou pozici.", "Boş yuvan yok."},   // You have no free slots available.
    {0x47512c0bu, "Γεμίζεις τα τυφέκια γρηγορότερα", "Rychleji přebíjíte dlouhé zbraně",
     "Uzun namlulu silahları daha hızlı doldurursun"},   // You reload long guns faster
    {0x8ba95edfu, "Γεμίζεις τα πιστόλια γρηγορότερα", "Rychleji přebíjíte pistole",
     "Tabancaları daha hızlı doldurursun"},   // You reload pistols faster
    {0xe2ce50dfu, "Zoran Lazarević", "Zoran Lazarević", "Zoran Lazarević"},   // Zoran Lazarević
    {0x4701ecf9u, "Zorskel", "Zorskel", "Zorskel"},   // Zorskel
    {0xb9ce5fc4u, "Απομένουν [A] προσπάθειες", "Zbývající pokusy: [A]", "[A] Deneme Kaldı"},   // [A] Tries Remaining
    {0x25616124u, "[A] κατάληψη", "[A] obsazení", "[A] ele geçirme"},   // [A] capture
    {0x9b537455u, "[A] καταλήψεις", "[A] obsazení", "[A] ele geçirme"},   // [A] captures
    {0x4afc6214u, "[A] εξοντώσεις", "Zabití: [A]", "[A] leş"},   // [A] kills
    {0xba8db526u, "[A] παίκτες στην ομάδα", "Hráči ve skupině: [A]", "Grupta [A] oyuncu"},   // [A] players in party
    {0x2b3077c4u, "[A] πόντος", "[A] bod", "[A] puan"},   // [A] point
    {0xf6cb345fu, "[A] πόντοι", "Body: [A]", "[A] puan"},   // [A] points
    {0x5c0f6a01u, "Ο [TEXT] σκόραρε!", "[TEXT] skóroval!", "[TEXT] sayı yaptı!"},   // [TEXT] Scored!
    {0x7b4d4744u, "κατέλαβε μια ζώνη", "obsadil/a zónu", "bir bölgeyi ele geçirdi"},   // captured a zone
    {0x79e31b79u, "κατέλαβε τον θησαυρό", "získal/a poklad", "hazineyi ele geçirdi"},   // captured the treasure
    {0xe45fc8a8u, "έκλεψε", "podváděl/a", "hile yaptı"},   // cheated
    {0xf685fae1u, "άφησε τον θησαυρό", "upustil/a poklad", "hazineyi düşürdü"},   // dropped the treasure
    {0xdd8bb2dbu, "αποχώρησε από το παιχνίδι", "opustil/a hru", "oyundan ayrıldı"},   // left the game
    {0xfcfed5d0u, "έχασε μια ζώνη", "ztratil/a zónu", "bir bölgeyi kaybetti"},   // lost a zone
    {0x156e815fu, "σήκωσε τον θησαυρό", "sebral/a poklad", "hazineyi aldı"},   // picked up the treasure
    {0x9f4fd672u, "αποβλήθηκε επειδή σκότωσε συμπαίκτη", "byl/a vyhozen/a za zabití spoluhráče",
     "bir takım arkadaşını öldürdüğü için atıldı"},   // was kicked for killing a teammate
    {0x8187843du, "|C|", "|C|", "|C|"},   // |C|
    {0x40133004u, "|R1|", "|R1|", "|R1|"},   // |R1|
    {0xe2186a0cu, "|SELECT| Πίνακας βαθμολογίας    |D-PAD| Κάμερα", "|SELECT| Výsledková tabulka    |D-PAD| Kamera",
     "|SELECT| Skor Tablosu    |D-PAD| Kamera"},   // |SELECT| Scoreboard    |D-PAD| Camera
    {0x660f6bdau, "|S|", "|S|", "|S|"},   // |S|
    {0x97bf5532u, "|S| ή |C|", "|S| nebo |C|", "|S| veya |C|"},   // |S| or |C|
    {0xa4d72e3eu, "|S| ή |C| ή |R1|", "|S| nebo |C| nebo |R1|", "|S|, |C| veya |R1|"},   // |S| or |C| or |R1|
};

TextFindFn orig_text_find = nullptr;
thread_local int find_depth = 0;   // TextDb::Find calls itself for the next bank: only the outer call
std::atomic<int> current_language{-1};
char player_text[256];

std::mutex text_lock;
// imported banks by language, loaded once each and never freed (the game keeps the pointers)
std::unordered_map<int, std::unordered_map<uint32_t, std::string>> imported;
std::unordered_set<uint64_t> reported;

template <typename T> T field(const void *p, uint64_t off) {
    T v;
    std::memcpy(&v, static_cast<const uint8_t *>(p) + off, sizeof v);
    return v;
}
bool placeholder(const char *s) { return s && s[0] == '_' && s[1] == 0; }
int column(int language) { return language >= 0 && language < kLanguages ? kColumnOf[language] : EN; }

// The language number of the bank chain `bank` (-1: none loaded).
int bank_language(const uint8_t *bank) {
    for (int guard = 0; bank && guard < 8; ++guard, bank = *reinterpret_cast<uint8_t *const *>(bank + TB_NEXT)) {
        const int32_t index = field<int32_t>(bank, TB_LANGUAGE);
        if (index >= 0 && index < 64) return *image.at<int32_t>(kLanguageTable + uint64_t(index) * 0x38 + 0x14);
    }
    return -1;
}

uint32_t be32(const uint8_t *p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

// A bank file (big-endian: u32 count; count x {u32 id, u32 offset}; NUL-terminated strings).
int load_bank(const std::string &guest, std::unordered_map<uint32_t, std::string> &out) {
    char host[1024];
    if (runtime_file_translate(guest.c_str(), host, sizeof host) != 0) return -1;
    std::FILE *f = std::fopen(host, "rb");
    if (!f) return -1;
    std::string d;
    char buffer[65536];
    for (size_t n; (n = std::fread(buffer, 1, sizeof buffer, f)) > 0;) d.append(buffer, n);
    std::fclose(f);
    const auto *p = reinterpret_cast<const uint8_t *>(d.data());
    if (d.size() < 4) return -1;
    const uint32_t count = be32(p);
    const uint64_t blob = 4 + 8 * uint64_t(count);
    if (blob > d.size()) return -1;
    int added = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t id = be32(p + 4 + 8 * i), off = be32(p + 8 + 8 * i);
        if (blob + off >= d.size()) continue;
        const char *s = d.data() + blob + off;
        out[id].assign(s, strnlen(s, d.size() - size_t(blob + off)));
        ++added;
    }
    return added;
}

// The imported text of `id` in `language` (NULL: none, or only the placeholder). Under text_lock.
const char *imported_text(int language, uint32_t id) {
    auto it = imported.find(language);
    if (it == imported.end()) {
        it = imported.emplace(language, std::unordered_map<uint32_t, std::string>{}).first;
        int files = 0;
        for (const char *category : {"common", "networking"})   // networking wins for ids both have
            if (load_bank(kTextDir + std::to_string(language) + "." + category, it->second) >= 0) ++files;
        log("texts: language %d: %zu imported texts from %d bank(s)%s", language, it->second.size(), files,
            files ? "" : " (none: run the multiplayer import)");
    }
    const auto t = it->second.find(id);
    return t == it->second.end() || placeholder(t->second.c_str()) ? nullptr : t->second.c_str();
}

const char *own(uint32_t id, int language) {
    if (id == kTextPlayer) return player_text;
    const int c = column(language);
    for (const Text &t : kOwn) {
        if (t.id != id) continue;
        if (t.text[c]) return t.text[c];
        if (c == ES419 && t.text[ES]) return t.text[ES];
        if (c == PTBR && t.text[PT]) return t.text[PT];
        return t.text[EN];
    }
    if (c == EL || c == CS || c == TR) {
        static const std::unordered_map<uint32_t, const MenuText *> menu = [] {
            std::unordered_map<uint32_t, const MenuText *> m;
            for (const MenuText &t : kMenu) m.emplace(t.id, &t);
            return m;
        }();
        const auto it = menu.find(id);
        if (it != menu.end()) return c == EL ? it->second->el : c == CS ? it->second->cs : it->second->tr;
    }
    return nullptr;
}

// Once per id and language. Under text_lock.
bool first_report(int language, uint32_t id) {
    return reported.insert(uint64_t(uint32_t(language)) << 32 | id).second;
}

// Steps 2..7 for an id the game's banks miss (`game`: NULL, or its placeholder).
const char *resolve(void *db, uint32_t id, int language, const char *game, int hops) {
    for (const auto &a : kAliases) {
        if (a.id != id || hops > 3) continue;
        const char *t = orig_text_find(db, a.to);
        return t && !placeholder(t) ? t : resolve(db, a.to, language, game ? game : t, hops + 1);
    }
    std::lock_guard<std::mutex> lock(text_lock);
    if (const char *t = imported_text(language, id)) return t;
    if (const char *t = own(id, language)) return t;
    if (const char *t = unlocks_text(id)) return t;
    if (language != kEnglish) {
        if (const char *t = imported_text(kEnglish, id)) {
            if (first_report(language, id)) log("texts: %08x shown in English (language %d)", id, language);
            return t;
        }
    }
    if (first_report(language, id))
        log("texts: no text for %08x (language %d)%s", id, language, game ? ", the game's placeholder" : "");
    return game;
}

}  // namespace

U2MP_GUEST const char *text_find(void *db, uint32_t id) {
    if (!orig_text_find) return nullptr;
    if (find_depth > 0) return orig_text_find(db, id);   // the walk through the next banks
    ++find_depth;
    const char *game = orig_text_find(db, id);
    --find_depth;
    if (game && !placeholder(game)) return game;
    // sid("none") (0xf114b39d) is the game's "no text" id: most readers test for it (0x5da851, 0x5aceaa),
    // one in-match path looked it up (run 42). It is empty text, not a missing one.
    if (id == 0xf114b39du) return "";
    int language = bank_language(static_cast<const uint8_t *>(db));
    if (language < 0) language = kEnglish;
    current_language.store(language, std::memory_order_relaxed);
    ++find_depth;
    const char *text = resolve(db, id, language, game, 0);
    --find_depth;
    return text;
}

void text_find_original(TextFindFn original) { orig_text_find = original; }

const char *own_text(uint32_t id) {
    const int language = current_language.load(std::memory_order_relaxed);
    return own(id, language < 0 ? kEnglish : language);
}

void text_set_player(const char *text) { std::snprintf(player_text, sizeof player_text, "%s", text); }

}  // namespace u2mp
