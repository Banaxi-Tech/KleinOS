// Chess: full legal move generation (castling, en passant, promotion, check,
// checkmate, stalemate, repetition, 50-move rule) against a small alpha-beta AI.
#include <initializer_list>
#include "ui/window.h"
#include "ui/widgets.h"
#include "ui/app.h"
#include "ui/theme.h"
#include "arch/cpu.h"
#include "arch/interrupts.h"
#include "lib/math.h"

namespace {

// ---------------------------------------------------------------- board model
enum : int8_t { EMPTY = 0, PAWN = 1, KNIGHT = 2, BISHOP = 3, ROOK = 4, QUEEN = 5, KING = 6 };
enum : uint8_t { MF_CASTLE = 1, MF_EP = 2, MF_DOUBLE = 4, MF_CAPTURE = 8 };

constexpr int kMaxMoves = 224;
constexpr int kMaxPly = 16;
constexpr int kMate = 100000;
constexpr int kInf = 1000000;

struct Move {
    int8_t from, to, promo;
    uint8_t flags;
    int16_t score;
};

struct MoveList {
    Move m[kMaxMoves];
    int n = 0;
    void add(int from, int to, int promo, int flags) {
        Move& x = m[n++];
        x.from = (int8_t)from;
        x.to = (int8_t)to;
        x.promo = (int8_t)promo;
        x.flags = (uint8_t)flags;
        x.score = 0;
    }
};

// Piece codes: positive = white, negative = black.
struct Pos {
    int8_t b[64];
    int8_t side;      // +1 white to move, -1 black
    uint8_t castle;   // 1 WK, 2 WQ, 4 BK, 8 BQ
    int8_t ep;        // en passant target square or -1
    uint16_t half;    // halfmove clock
    uint16_t full;    // fullmove number
};

inline int rank_of(int s) { return s >> 3; }
inline int file_of(int s) { return s & 7; }

const int kKnightD[8][2] = {{1, 2}, {2, 1}, {2, -1}, {1, -2}, {-1, -2}, {-2, -1}, {-2, 1}, {-1, 2}};
const int kKingD[8][2] = {{1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}, {-1, -1}, {0, -1}, {1, -1}};
const int kRookD[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
const int kBishD[4][2] = {{1, 1}, {1, -1}, {-1, 1}, {-1, -1}};

Pos start_pos() {
    Pos p;
    memset(&p, 0, sizeof(p));
    const int8_t back[8] = {ROOK, KNIGHT, BISHOP, QUEEN, KING, BISHOP, KNIGHT, ROOK};
    for (int f = 0; f < 8; f++) {
        p.b[f] = back[f];
        p.b[8 + f] = PAWN;
        p.b[48 + f] = -PAWN;
        p.b[56 + f] = -back[f];
    }
    p.side = 1;
    p.castle = 15;
    p.ep = -1;
    p.half = 0;
    p.full = 1;
    return p;
}

bool same_pos(const Pos& a, const Pos& b) {
    return memcmp(a.b, b.b, 64) == 0 && a.side == b.side && a.castle == b.castle && a.ep == b.ep;
}

// True if square sq is attacked by any piece of colour `by`.
bool attacked(const Pos& p, int sq, int by) {
    int r = rank_of(sq), f = file_of(sq);
    int pr = r - by;   // pawns of `by` attack from the rank behind sq (from their side)
    if (pr >= 0 && pr < 8) {
        for (int df = -1; df <= 1; df += 2) {
            int pf = f + df;
            if (pf >= 0 && pf < 8 && p.b[pr * 8 + pf] == by * PAWN) return true;
        }
    }
    for (auto& d : kKnightD) {
        int nr = r + d[0], nf = f + d[1];
        if (nr >= 0 && nr < 8 && nf >= 0 && nf < 8 && p.b[nr * 8 + nf] == by * KNIGHT) return true;
    }
    for (auto& d : kKingD) {
        int nr = r + d[0], nf = f + d[1];
        if (nr >= 0 && nr < 8 && nf >= 0 && nf < 8 && p.b[nr * 8 + nf] == by * KING) return true;
    }
    for (int k = 0; k < 8; k++) {
        const int* d = k < 4 ? kRookD[k] : kBishD[k - 4];
        int want = k < 4 ? by * ROOK : by * BISHOP;
        int nr = r + d[0], nf = f + d[1];
        while (nr >= 0 && nr < 8 && nf >= 0 && nf < 8) {
            int pc = p.b[nr * 8 + nf];
            if (pc) {
                if (pc == want || pc == by * QUEEN) return true;
                break;
            }
            nr += d[0];
            nf += d[1];
        }
    }
    return false;
}

int find_king(const Pos& p, int side) {
    for (int s = 0; s < 64; s++)
        if (p.b[s] == side * KING) return s;
    return -1;
}

bool in_check(const Pos& p, int side) {
    int k = find_king(p, side);
    return k >= 0 && attacked(p, k, -side);
}

Pos make(const Pos& p, const Move& m) {
    Pos n = p;
    int from = m.from, to = m.to;
    int pc = n.b[from];
    int type = pc < 0 ? -pc : pc;
    int side = p.side;
    int captured = n.b[to];
    n.b[from] = 0;
    if (m.flags & MF_EP) {
        int cap = (from & ~7) | (to & 7);
        n.b[cap] = 0;
        captured = PAWN;
    }
    if (m.flags & MF_CASTLE) {
        if (to > from) { n.b[from + 1] = n.b[from + 3]; n.b[from + 3] = 0; }
        else           { n.b[from - 1] = n.b[from - 4]; n.b[from - 4] = 0; }
    }
    n.b[to] = m.promo ? (int8_t)(m.promo * side) : (int8_t)pc;

    if (type == KING) n.castle = (uint8_t)(n.castle & (side > 0 ? ~3 : ~12));
    if (from == 0 || to == 0) n.castle = (uint8_t)(n.castle & ~2);
    if (from == 7 || to == 7) n.castle = (uint8_t)(n.castle & ~1);
    if (from == 56 || to == 56) n.castle = (uint8_t)(n.castle & ~8);
    if (from == 63 || to == 63) n.castle = (uint8_t)(n.castle & ~4);

    n.ep = (type == PAWN && (to - from == 16 || from - to == 16)) ? (int8_t)((from + to) / 2) : (int8_t)-1;
    n.half = (type == PAWN || captured) ? 0 : (uint16_t)(n.half + 1);
    if (side < 0) n.full++;
    n.side = (int8_t)-side;
    return n;
}

void add_moves_for_target(MoveList& L, int from, int to, bool capture, int promo_rank) {
    if (rank_of(to) == promo_rank) L.add(from, to, QUEEN, capture ? MF_CAPTURE : 0);
    else L.add(from, to, 0, capture ? MF_CAPTURE : 0);
}

// Pseudo-legal moves (may leave own king in check) appended to L.
void gen_pseudo(const Pos& p, MoveList& L) {
    L.n = 0;
    int side = p.side;
    int promo_rank = side > 0 ? 7 : 0;
    for (int s = 0; s < 64; s++) {
        int pc = p.b[s];
        if (!pc || (pc > 0) != (side > 0)) continue;
        int type = pc < 0 ? -pc : pc;
        int r = rank_of(s), f = file_of(s);
        if (type == PAWN) {
            int dir = side * 8;
            int start = side > 0 ? 1 : 6;
            int one = s + dir;
            if (one >= 0 && one < 64 && p.b[one] == 0) {
                add_moves_for_target(L, s, one, false, promo_rank);
                int two = one + dir;
                if (r == start && p.b[two] == 0) L.add(s, two, 0, MF_DOUBLE);
            }
            int rr = r + side;
            if (rr >= 0 && rr < 8) {
                for (int df = -1; df <= 1; df += 2) {
                    int nf = f + df;
                    if (nf < 0 || nf > 7) continue;
                    int t = rr * 8 + nf;
                    if (p.b[t] && (p.b[t] > 0) != (side > 0)) add_moves_for_target(L, s, t, true, promo_rank);
                    else if (t == p.ep) L.add(s, t, 0, MF_EP | MF_CAPTURE);
                }
            }
        } else if (type == KNIGHT || type == KING) {
            const int(*d)[2] = type == KNIGHT ? kKnightD : kKingD;
            for (int k = 0; k < 8; k++) {
                int nr = r + d[k][0], nf = f + d[k][1];
                if (nr < 0 || nr > 7 || nf < 0 || nf > 7) continue;
                int t = nr * 8 + nf;
                if (p.b[t] && (p.b[t] > 0) == (side > 0)) continue;
                L.add(s, t, 0, p.b[t] ? MF_CAPTURE : 0);
            }
            if (type == KING) {
                int home = side > 0 ? 4 : 60;
                if (s == home && !attacked(p, s, -side)) {
                    bool ks = side > 0 ? (p.castle & 1) : (p.castle & 4);
                    if (ks && p.b[home + 1] == 0 && p.b[home + 2] == 0 && p.b[home + 3] == side * ROOK &&
                        !attacked(p, home + 1, -side) && !attacked(p, home + 2, -side))
                        L.add(s, home + 2, 0, MF_CASTLE);
                    bool qs = side > 0 ? (p.castle & 2) : (p.castle & 8);
                    if (qs && p.b[home - 1] == 0 && p.b[home - 2] == 0 && p.b[home - 3] == 0 &&
                        p.b[home - 4] == side * ROOK && !attacked(p, home - 1, -side) && !attacked(p, home - 2, -side))
                        L.add(s, home - 2, 0, MF_CASTLE);
                }
            }
        } else {
            int first = type == BISHOP ? 4 : 0;
            int last = type == ROOK ? 4 : 8;
            if (type == QUEEN) { first = 0; last = 8; }
            for (int k = first; k < last; k++) {
                const int* d = k < 4 ? kRookD[k] : kBishD[k - 4];
                int nr = r + d[0], nf = f + d[1];
                while (nr >= 0 && nr < 8 && nf >= 0 && nf < 8) {
                    int t = nr * 8 + nf;
                    if (p.b[t]) {
                        if ((p.b[t] > 0) != (side > 0)) L.add(s, t, 0, MF_CAPTURE);
                        break;
                    }
                    L.add(s, t, 0, 0);
                    nr += d[0];
                    nf += d[1];
                }
            }
        }
    }
}

// Fully legal moves in L.
void gen_legal(const Pos& p, MoveList& L) {
    gen_pseudo(p, L);
    int w = 0;
    for (int i = 0; i < L.n; i++) {
        Pos n = make(p, L.m[i]);
        if (!in_check(n, p.side)) L.m[w++] = L.m[i];
    }
    L.n = w;
}

bool insufficient(const Pos& p) {
    int minors = 0;
    for (int s = 0; s < 64; s++) {
        int t = p.b[s] < 0 ? -p.b[s] : p.b[s];
        if (t == PAWN || t == ROOK || t == QUEEN) return false;
        if (t == KNIGHT || t == BISHOP) minors++;
    }
    return minors <= 1;
}

// ---------------------------------------------------------------- evaluation
const int kValue[7] = {0, 100, 320, 330, 500, 900, 0};

// Piece-square tables from white's point of view, rank 8 first.
const int kPawnPst[64] = {
     0,  0,  0,  0,  0,  0,  0,  0,
    50, 50, 50, 50, 50, 50, 50, 50,
    10, 10, 20, 30, 30, 20, 10, 10,
     5,  5, 10, 25, 25, 10,  5,  5,
     0,  0,  0, 20, 20,  0,  0,  0,
     5, -5,-10,  0,  0,-10, -5,  5,
     5, 10, 10,-20,-20, 10, 10,  5,
     0,  0,  0,  0,  0,  0,  0,  0};
const int kKnightPst[64] = {
    -50,-40,-30,-30,-30,-30,-40,-50,
    -40,-20,  0,  0,  0,  0,-20,-40,
    -30,  0, 10, 15, 15, 10,  0,-30,
    -30,  5, 15, 20, 20, 15,  5,-30,
    -30,  0, 15, 20, 20, 15,  0,-30,
    -30,  5, 10, 15, 15, 10,  5,-30,
    -40,-20,  0,  5,  5,  0,-20,-40,
    -50,-40,-30,-30,-30,-30,-40,-50};
const int kBishopPst[64] = {
    -20,-10,-10,-10,-10,-10,-10,-20,
    -10,  0,  0,  0,  0,  0,  0,-10,
    -10,  0,  5, 10, 10,  5,  0,-10,
    -10,  5,  5, 10, 10,  5,  5,-10,
    -10,  0, 10, 10, 10, 10,  0,-10,
    -10, 10, 10, 10, 10, 10, 10,-10,
    -10,  5,  0,  0,  0,  0,  5,-10,
    -20,-10,-10,-10,-10,-10,-10,-20};
const int kRookPst[64] = {
      0,  0,  0,  0,  0,  0,  0,  0,
      5, 10, 10, 10, 10, 10, 10,  5,
     -5,  0,  0,  0,  0,  0,  0, -5,
     -5,  0,  0,  0,  0,  0,  0, -5,
     -5,  0,  0,  0,  0,  0,  0, -5,
     -5,  0,  0,  0,  0,  0,  0, -5,
     -5,  0,  0,  0,  0,  0,  0, -5,
      0,  0,  0,  5,  5,  0,  0,  0};
const int kQueenPst[64] = {
    -20,-10,-10, -5, -5,-10,-10,-20,
    -10,  0,  0,  0,  0,  0,  0,-10,
    -10,  0,  5,  5,  5,  5,  0,-10,
     -5,  0,  5,  5,  5,  5,  0, -5,
      0,  0,  5,  5,  5,  5,  0, -5,
    -10,  5,  5,  5,  5,  5,  0,-10,
    -10,  0,  5,  0,  0,  0,  0,-10,
    -20,-10,-10, -5, -5,-10,-10,-20};
const int kKingMidPst[64] = {
    -30,-40,-40,-50,-50,-40,-40,-30,
    -30,-40,-40,-50,-50,-40,-40,-30,
    -30,-40,-40,-50,-50,-40,-40,-30,
    -30,-40,-40,-50,-50,-40,-40,-30,
    -20,-30,-30,-40,-40,-30,-30,-20,
    -10,-20,-20,-20,-20,-20,-20,-10,
     20, 20,  0,  0,  0,  0, 20, 20,
     20, 30, 10,  0,  0, 10, 30, 20};
const int kKingEndPst[64] = {
    -50,-40,-30,-20,-20,-30,-40,-50,
    -30,-20,-10,  0,  0,-10,-20,-30,
    -30,-10, 20, 30, 30, 20,-10,-30,
    -30,-10, 30, 40, 40, 30,-10,-30,
    -30,-10, 30, 40, 40, 30,-10,-30,
    -30,-10, 20, 30, 30, 20,-10,-30,
    -30,-30,  0,  0,  0,  0,-30,-30,
    -50,-30,-30,-30,-30,-30,-30,-50};

// Score from the side-to-move's perspective.
int eval_pos(const Pos& p) {
    int nonpawn = 0;
    for (int s = 0; s < 64; s++) {
        int t = p.b[s] < 0 ? -p.b[s] : p.b[s];
        if (t >= KNIGHT && t <= QUEEN) nonpawn += kValue[t];
    }
    bool endgame = nonpawn <= 1300;
    int score = 0;
    for (int s = 0; s < 64; s++) {
        int pc = p.b[s];
        if (!pc) continue;
        int t = pc < 0 ? -pc : pc;
        int side = pc > 0 ? 1 : -1;
        int r = rank_of(s), f = file_of(s);
        int idx = side > 0 ? ((7 - r) * 8 + f) : (r * 8 + f);
        int pst = 0;
        switch (t) {
            case PAWN: pst = kPawnPst[idx]; break;
            case KNIGHT: pst = kKnightPst[idx]; break;
            case BISHOP: pst = kBishopPst[idx]; break;
            case ROOK: pst = kRookPst[idx]; break;
            case QUEEN: pst = kQueenPst[idx]; break;
            case KING: pst = endgame ? kKingEndPst[idx] : kKingMidPst[idx]; break;
        }
        score += side * (kValue[t] + pst);
    }
    return p.side * score;
}

// ---------------------------------------------------------------- search
MoveList g_lists[kMaxPly];
int g_nodes = 0;
uint64_t g_deadline = 0;
bool g_abort = false;

void score_moves(const Pos& p, MoveList& L) {
    for (int i = 0; i < L.n; i++) {
        Move& m = L.m[i];
        int s = 0;
        if (m.flags & MF_CAPTURE) {
            int victim = (m.flags & MF_EP) ? PAWN : (p.b[m.to] < 0 ? -p.b[m.to] : p.b[m.to]);
            int attacker = p.b[m.from] < 0 ? -p.b[m.from] : p.b[m.from];
            s = 10 * kValue[victim] - attacker + 5000;
        }
        if (m.promo) s += 4000;
        m.score = (int16_t)s;
    }
}

void pick_best(MoveList& L, int i) {
    int best = i;
    for (int j = i + 1; j < L.n; j++)
        if (L.m[j].score > L.m[best].score) best = j;
    if (best != i) {
        Move t = L.m[i];
        L.m[i] = L.m[best];
        L.m[best] = t;
    }
}

int quiesce(Pos& p, int alpha, int beta, int ply, int qd) {
    if (((++g_nodes) & 511) == 0 && timer_ms() > g_deadline) g_abort = true;
    if (g_abort) return 0;
    int stand = eval_pos(p);
    if (qd >= 4 || ply >= kMaxPly - 1) return stand;
    if (stand >= beta) return stand;
    if (stand > alpha) alpha = stand;
    MoveList& L = g_lists[ply];
    gen_legal(p, L);
    int w = 0;
    for (int i = 0; i < L.n; i++)
        if (L.m[i].flags & MF_CAPTURE) L.m[w++] = L.m[i];
    L.n = w;
    score_moves(p, L);
    for (int i = 0; i < L.n; i++) {
        pick_best(L, i);
        Pos n = make(p, L.m[i]);
        int v = -quiesce(n, -beta, -alpha, ply + 1, qd + 1);
        if (g_abort) return 0;
        if (v >= beta) return v;
        if (v > alpha) alpha = v;
    }
    return alpha;
}

int negamax(Pos& p, int depth, int alpha, int beta, int ply) {
    if (((++g_nodes) & 511) == 0 && timer_ms() > g_deadline) g_abort = true;
    if (g_abort) return 0;
    if (p.half >= 100 || insufficient(p)) return 0;
    if (depth <= 0) return quiesce(p, alpha, beta, ply, 0);
    MoveList& L = g_lists[ply];
    gen_legal(p, L);
    if (L.n == 0) return in_check(p, p.side) ? -(kMate - ply) : 0;
    score_moves(p, L);
    int best = -kInf;
    for (int i = 0; i < L.n; i++) {
        pick_best(L, i);
        Pos n = make(p, L.m[i]);
        int v = -negamax(n, depth - 1, -beta, -alpha, ply + 1);
        if (g_abort) return 0;
        if (v > best) best = v;
        if (v > alpha) alpha = v;
        if (alpha >= beta) break;
    }
    return best;
}

// Searches every root move at one depth. Returns false if aborted or no moves.
bool root_search(const Pos& p, int depth, Move& out, int& score) {
    MoveList& L = g_lists[0];
    gen_legal(p, L);
    if (L.n == 0) return false;
    score_moves(p, L);
    // Remember the move itself: pick_best reorders L, so an index would go stale.
    // Full window per root move: a narrowed window would return bounds that look like exact scores.
    int best = -kInf, ties = 0;
    Move best_move = L.m[0];
    for (int i = 0; i < L.n; i++) {
        pick_best(L, i);
        Pos n = make(p, L.m[i]);
        int v = -negamax(n, depth - 1, -kInf, kInf, 1);
        if (g_abort) return false;
        if (v > best) {
            best = v;
            best_move = L.m[i];
            ties = 1;
        } else if (v == best) {
            ties++;
            if (cpu_random64() % (uint64_t)ties == 0) best_move = L.m[i];   // random tie-break
        }
    }
    out = best_move;
    score = best;
    return true;
}

// Iterative deepening (max depth 3) inside a fixed time budget.
bool choose_move(const Pos& p, Move& out) {
    g_nodes = 0;
    g_abort = false;
    g_deadline = timer_ms() + 260;
    bool have = false;
    for (int d = 1; d <= 3; d++) {
        Move m;
        int sc = 0;
        if (!root_search(p, d, m, sc)) break;
        out = m;
        have = true;
        if (sc > kMate / 2 || sc < -kMate / 2) break;   // forced mate found
    }
    return have;
}

// ---------------------------------------------------------------- notation
void make_san(const Pos& p, const Move& m, char* out, size_t cap) {
    Pos after = make(p, m);
    int type = p.b[m.from] < 0 ? -p.b[m.from] : p.b[m.from];
    bool capture = (m.flags & (MF_CAPTURE | MF_EP)) || p.b[m.to];
    char buf[16];
    int k = 0;
    if (m.flags & MF_CASTLE) {
        buf[k++] = 'O';
        buf[k++] = '-';
        buf[k++] = 'O';
        if (m.to < m.from) { buf[k++] = '-'; buf[k++] = 'O'; }
    } else {
        if (type == PAWN) {
            if (capture) buf[k++] = (char)('a' + file_of(m.from));
        } else {
            static const char letters[] = "?PNBRQK";
            buf[k++] = letters[type];
            MoveList& L = g_lists[0];
            gen_legal(p, L);
            bool other = false, same_file = false, same_rank = false;
            for (int i = 0; i < L.n; i++) {
                const Move& o = L.m[i];
                if (o.to != m.to || o.from == m.from) continue;
                int ot = p.b[o.from] < 0 ? -p.b[o.from] : p.b[o.from];
                if (ot != type) continue;
                other = true;
                if (file_of(o.from) == file_of(m.from)) same_file = true;
                if (rank_of(o.from) == rank_of(m.from)) same_rank = true;
            }
            if (other) {
                if (!same_file) buf[k++] = (char)('a' + file_of(m.from));
                else if (!same_rank) buf[k++] = (char)('1' + rank_of(m.from));
                else { buf[k++] = (char)('a' + file_of(m.from)); buf[k++] = (char)('1' + rank_of(m.from)); }
            }
        }
        if (capture) {
            buf[k++] = 'x';
        }
        buf[k++] = (char)('a' + file_of(m.to));
        buf[k++] = (char)('1' + rank_of(m.to));
        if (m.promo) { buf[k++] = '='; buf[k++] = 'Q'; }
    }
    if (in_check(after, after.side)) {
        MoveList& L = g_lists[0];
        gen_legal(after, L);
        buf[k++] = L.n == 0 ? '#' : '+';
    }
    buf[k] = 0;
    strlcpy(out, buf, cap);
}

// ---------------------------------------------------------------- drawing helpers
constexpr int kBoardX = 44, kBoardY = 22, kSq = 56;

void pc_poly(Canvas& c, Rect r, std::initializer_list<float> uv, Color body, Color edge) {
    PointF pts[16] = {};
    const float* u = uv.begin();
    int n = (int)uv.size() / 2;
    for (int i = 0; i < n; i++) pts[i] = PointF{r.x + u[2 * i] * r.w, r.y + u[2 * i + 1] * r.h};
    c.fill_polygon(pts, n, body);
    for (int i = 0; i < n; i++) {
        PointF a = pts[i], b = pts[(i + 1) % n];
        c.draw_line(a.x, a.y, b.x, b.y, edge, 1.0f);
    }
}

void pc_disc(Canvas& c, Rect r, float u, float v, float rad, Color body, Color edge) {
    float cx = r.x + u * r.w, cy = r.y + v * r.h, rr = rad * r.w;
    c.fill_circle(cx, cy, rr, body);
    c.stroke_circle(cx, cy, rr, edge, 1.0f);
}

void pc_box(Canvas& c, Rect r, float u, float v, float w, float h, Color body, Color edge) {
    Rect b((int)(r.x + u * r.w), (int)(r.y + v * r.h), (int)(w * r.w), (int)(h * r.h));
    c.fill_round_rect(b, 2, body);
    c.stroke_round_rect(b, 2, edge, 1.0f);
}

// Vector chess piece drawn inside square r. pc: +1..+6 white, -1..-6 black.
void draw_piece(Canvas& c, Rect r, int pc) {
    bool white = pc > 0;
    int t = pc < 0 ? -pc : pc;
    Color body = white ? hex(0xF7F7F2) : hex(0x30353E);
    Color edge = white ? rgba(30, 34, 40, 170) : rgba(0, 0, 0, 200);
    Color detail = white ? hex(0x2A2F38) : hex(0xF2F2EE);

    c.fill_ellipse(r.x + 0.5f * r.w, r.y + 0.9f * r.h, 0.34f * r.w, 0.05f * r.h, rgba(0, 0, 0, 70));
    pc_box(c, r, 0.2f, 0.8f, 0.6f, 0.08f, body, edge);

    switch (t) {
        case PAWN: {
            pc_poly(c, r, {0.37f, 0.46f, 0.63f, 0.46f, 0.7f, 0.74f, 0.3f, 0.74f}, body, edge);
            pc_box(c, r, 0.28f, 0.72f, 0.44f, 0.07f, body, edge);
            pc_disc(c, r, 0.5f, 0.34f, 0.14f, body, edge);
            break;
        }
        case ROOK: {
            pc_poly(c, r, {0.3f, 0.3f, 0.7f, 0.3f, 0.67f, 0.76f, 0.33f, 0.76f}, body, edge);
            pc_poly(c, r, {0.28f, 0.3f, 0.28f, 0.12f, 0.41f, 0.12f, 0.41f, 0.22f, 0.59f, 0.22f,
                                          0.59f, 0.12f, 0.72f, 0.12f, 0.72f, 0.3f}, body, edge);
            pc_box(c, r, 0.26f, 0.72f, 0.48f, 0.07f, body, edge);
            break;
        }
        case BISHOP: {
            pc_poly(c, r, {0.36f, 0.42f, 0.64f, 0.42f, 0.7f, 0.74f, 0.3f, 0.74f}, body, edge);
            pc_disc(c, r, 0.5f, 0.31f, 0.13f, body, edge);
            pc_disc(c, r, 0.5f, 0.15f, 0.05f, body, edge);
            c.draw_line(r.x + 0.5f * r.w, r.y + 0.22f * r.h, r.x + 0.6f * r.w, r.y + 0.34f * r.h, detail, 1.6f);
            break;
        }
        case KNIGHT: {
            pc_poly(c, r, {0.26f, 0.8f, 0.74f, 0.8f, 0.74f, 0.62f, 0.64f, 0.44f, 0.68f, 0.24f,
                                          0.6f, 0.12f, 0.45f, 0.09f, 0.3f, 0.2f, 0.24f, 0.34f, 0.16f, 0.42f,
                                          0.3f, 0.48f, 0.36f, 0.58f, 0.26f, 0.62f}, body, edge);
            pc_disc(c, r, 0.52f, 0.27f, 0.035f, detail, detail);
            break;
        }
        case QUEEN: {
            pc_poly(c, r, {0.3f, 0.74f, 0.7f, 0.74f, 0.66f, 0.42f, 0.34f, 0.42f}, body, edge);
            pc_poly(c, r, {0.32f, 0.44f, 0.2f, 0.2f, 0.36f, 0.32f, 0.5f, 0.13f, 0.64f, 0.32f,
                                          0.8f, 0.2f, 0.68f, 0.44f}, body, edge);
            pc_disc(c, r, 0.2f, 0.2f, 0.045f, body, edge);
            pc_disc(c, r, 0.5f, 0.13f, 0.05f, body, edge);
            pc_disc(c, r, 0.8f, 0.2f, 0.045f, body, edge);
            break;
        }
        case KING: {
            pc_poly(c, r, {0.3f, 0.74f, 0.7f, 0.74f, 0.64f, 0.42f, 0.36f, 0.42f}, body, edge);
            pc_box(c, r, 0.465f, 0.08f, 0.07f, 0.24f, body, edge);
            pc_box(c, r, 0.38f, 0.14f, 0.24f, 0.07f, body, edge);
            break;
        }
    }
}

// Icon: a white king on blue-gray glass.
void icon(Canvas& c, Rect r) {
    icon_base(c, r, hex(0x6F8CA3), hex(0x2B4459));
    draw_piece(c, r.inset(r.w / 6), KING);
}

Window* create();

}  // namespace

// ---------------------------------------------------------------- window
namespace {

class ChessWindow : public Window {
public:
    ChessWindow() : Window("Chess", 760, 520, WF_NO_ZOOM) {
        color_seg = (SegmentedControl*)add(new SegmentedControl(Rect(516, 92, 220, 30), {"White", "Black"}));
        color_seg->selected = 0;
        new_btn = (Button*)add(new Button(Rect(516, 132, 220, 32), "New Game", Button::Primary));
        new_btn->on_click = [this] { new_game(color_seg->selected == 0 ? 1 : -1); };
        undo_btn = (Button*)add(new Button(Rect(516, 172, 220, 28), "Undo Move"));
        undo_btn->on_click = [this] { undo(); };
        moves = (ListView*)add(new ListView(Rect(516, 216, 220, 280)));
        moves->row_height = 24;
        status_lbl = (Label*)add(new Label(Rect(516, 16, 220, 22), "", 16, Font::Bold));
        sub_lbl = (Label*)add(new Label(Rect(516, 40, 220, 18), "", 12, Font::Regular));
        (void)add(new Label(Rect(516, 72, 220, 16), "Play as", 12, Font::Medium));
        new_game(1);
        set_ticking(true);
    }

    void on_paint(Canvas& c) override {
        Window::on_paint(c);
        paint_board(c);
    }

    void on_mouse_down(int x, int y, int button) override {
        int s = square_at(x, y);
        if (s < 0) {
            Window::on_mouse_down(x, y, button);
            return;
        }
        click_square(s);
    }

    void on_tick(uint64_t now) override {
        if (ai_due && now >= ai_due && !game_over && pos.side == -human_side) {
            ai_due = 0;
            Move m;
            if (choose_move(pos, m)) play(m);
            else update_status();
            invalidate();
        }
    }

private:
    SegmentedControl* color_seg = nullptr;
    Button* new_btn = nullptr;
    Button* undo_btn = nullptr;
    ListView* moves = nullptr;
    Label* status_lbl = nullptr;
    Label* sub_lbl = nullptr;

    struct Entry {
        Pos before;
        Move m;
        String san;
    };
    Pos pos{};
    Vector<Entry> hist;
    int human_side = 1;
    bool game_over = false;
    uint64_t ai_due = 0;
    int sel = -1;
    MoveList sel_moves;
    MoveList ui_moves;
    int last_from = -1, last_to = -1;
    int check_sq = -1;

    bool flipped() const { return human_side < 0; }

    int square_at(int x, int y) const {
        if (x < kBoardX || y < kBoardY || x >= kBoardX + 8 * kSq || y >= kBoardY + 8 * kSq) return -1;
        int dc = (x - kBoardX) / kSq, dr = (y - kBoardY) / kSq;
        int file = flipped() ? 7 - dc : dc;
        int rank = flipped() ? dr : 7 - dr;
        return rank * 8 + file;
    }

    Rect square_rect(int s) const {
        int file = file_of(s), rank = rank_of(s);
        int dc = flipped() ? 7 - file : file;
        int dr = flipped() ? rank : 7 - rank;
        return Rect(kBoardX + dc * kSq, kBoardY + dr * kSq, kSq, kSq);
    }

    void new_game(int human) {
        human_side = human;
        pos = start_pos();
        hist.clear();
        clear_selection();
        ai_due = 0;
        last_from = last_to = -1;
        game_over = false;
        rebuild_list();
        update_status();
        if (pos.side != human_side) ai_due = timer_ms() + 450;
        invalidate();
    }

    void clear_selection() {
        sel = -1;
        sel_moves.n = 0;
    }

    void click_square(int s) {
        if (game_over || pos.side != human_side) return;
        if (sel >= 0) {
            for (int i = 0; i < sel_moves.n; i++) {
                if (sel_moves.m[i].to == s) {
                    Move m = sel_moves.m[i];
                    clear_selection();
                    play(m);
                    if (!game_over) ai_due = timer_ms() + 350;
                    invalidate();
                    return;
                }
            }
        }
        int pc = pos.b[s];
        if (pc && (pc > 0) == (human_side > 0)) {
            sel = s;
            sel_moves.n = 0;
            gen_legal(pos, ui_moves);
            for (int i = 0; i < ui_moves.n; i++)
                if (ui_moves.m[i].from == s) sel_moves.m[sel_moves.n++] = ui_moves.m[i];
        } else {
            clear_selection();
        }
        invalidate();
    }

    void play(const Move& m) {
        Entry e;
        e.before = pos;
        e.m = m;
        char san[16];
        make_san(pos, m, san, sizeof(san));
        e.san = san;
        hist.push(e);
        pos = make(pos, m);
        last_from = m.from;
        last_to = m.to;
        rebuild_list();
        update_status();
    }

    void undo() {
        if (hist.empty()) return;
        clear_selection();
        ai_due = 0;
        do {
            pos = hist.back().before;
            hist.pop();
        } while (!hist.empty() && pos.side != human_side);
        last_from = hist.empty() ? -1 : hist.back().m.from;
        last_to = hist.empty() ? -1 : hist.back().m.to;
        game_over = false;
        rebuild_list();
        update_status();
        invalidate();
    }

    int repetitions() const {
        int n = 1;
        for (size_t i = 0; i < hist.size(); i++)
            if (same_pos(hist[i].before, pos)) n++;
        return n;
    }

    void update_status() {
        gen_legal(pos, ui_moves);
        bool check = in_check(pos, pos.side);
        check_sq = check ? find_king(pos, pos.side) : -1;
        const char* title = "";
        char sub[64] = "";
        game_over = false;
        bool human_turn = pos.side == human_side;
        if (ui_moves.n == 0) {
            game_over = true;
            if (check) title = human_turn ? "Checkmate. AI wins." : "Checkmate. You win!";
            else title = "Stalemate. Draw.";
        } else if (pos.half >= 100) {
            game_over = true;
            title = "Draw by the 50-move rule.";
        } else if (insufficient(pos)) {
            game_over = true;
            title = "Draw. Not enough material.";
        } else if (repetitions() >= 3) {
            game_over = true;
            title = "Draw by threefold repetition.";
        } else {
            title = human_turn ? "Your move" : "AI is thinking...";
            if (check) snprintf(sub, sizeof(sub), "%s is in check", human_turn ? "You" : "AI");
            else snprintf(sub, sizeof(sub), "%s to move", pos.side > 0 ? "White" : "Black");
        }
        if (game_over) snprintf(sub, sizeof(sub), "Move %d", pos.full);
        status_lbl->set_text(title);
        sub_lbl->set_text(sub);
    }

    void rebuild_list() {
        moves->clear();
        char t[32];
        for (size_t i = 0; i < hist.size(); i++) {
            if (i % 2 == 0) {
                snprintf(t, sizeof(t), "%d.  %s", (int)(i / 2 + 1), hist[i].san.c_str());
                moves->add(t, "");
            } else {
                moves->items.back().detail = hist[i].san.c_str();
            }
        }
        if (!moves->items.empty()) moves->ensure_visible((int)moves->items.size() - 1);
    }

    void paint_board(Canvas& c) {
        const Theme& t = theme();
        Rect frame(kBoardX - 6, kBoardY - 6, 8 * kSq + 12, 8 * kSq + 12);
        c.draw_shadow(frame, 8, 12, rgba(0, 0, 0, 60), 0, 4);
        c.fill_round_rect(frame, 8, hex(0x5B7284));

        for (int s = 0; s < 64; s++) {
            Rect r = square_rect(s);
            int file = file_of(s), rank = rank_of(s);
            bool light = ((file + rank) & 1) != 0;
            c.fill_rect(r, light ? hex(0xE4EBF0) : hex(0x8CA1B1));
            if (s == last_from || s == last_to) c.fill_rect(r, rgba(246, 206, 92, 120));
            if (s == check_sq) {
                c.fill_circle(r.x + r.w / 2.0f, r.y + r.h / 2.0f, r.w * 0.62f, rgba(255, 60, 60, 60));
                c.fill_circle(r.x + r.w / 2.0f, r.y + r.h / 2.0f, r.w * 0.42f, rgba(255, 60, 60, 110));
            }
            if (s == sel) c.fill_rect(r, rgba(40, 120, 255, 130));
        }

        for (int i = 0; i < sel_moves.n; i++) {
            Rect r = square_rect(sel_moves.m[i].to);
            float cx = r.x + r.w / 2.0f, cy = r.y + r.h / 2.0f;
            bool capture = pos.b[sel_moves.m[i].to] || (sel_moves.m[i].flags & MF_EP);
            if (capture) c.stroke_circle(cx, cy, r.w / 2.0f - 4, rgba(20, 30, 40, 130), 3.0f);
            else c.fill_circle(cx, cy, 8.0f, rgba(20, 30, 40, 100));
        }

        for (int s = 0; s < 64; s++) {
            if (pos.b[s]) draw_piece(c, square_rect(s).inset(3), pos.b[s]);
        }

        // coordinates
        for (int i = 0; i < 8; i++) {
            int file = flipped() ? 7 - i : i;
            int rank = flipped() ? i : 7 - i;
            char f[2] = {(char)('a' + file), 0};
            char rk[2] = {(char)('1' + rank), 0};
            c.draw_text_in(Rect(kBoardX + i * kSq, kBoardY + 8 * kSq + 4, kSq - 4, 16), f, t.text_secondary, 12,
                           Font::Medium, Align::Right);
            c.draw_text_in(Rect(kBoardX - 22, kBoardY + i * kSq + 4, 14, 16), rk, t.text_secondary, 12,
                           Font::Medium, Align::Right);
        }
    }
};

Window* create() { return new ChessWindow(); }

AppRegistrar reg({"chess", "Chess", icon, create, "Games", true, false, 100,
                  "Play chess against the computer"});

}  // namespace
