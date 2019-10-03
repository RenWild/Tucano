/*-------------------------------------------------------------------------------
  tucano is a XBoard chess playing engine developed by Alcides Schulz.
  Copyright (C) 2011-present - Alcides Schulz

  tucano is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  tucano is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You can find the GNU General Public License at http://www.gnu.org/licenses/
-------------------------------------------------------------------------------*/

#include "globals.h"

//-------------------------------------------------------------------------------------------------
//  Zero window search.
//-------------------------------------------------------------------------------------------------

#define STAT_NULL_DEPTH 4
int STAT_NULL_MARGIN[STAT_NULL_DEPTH] = { 0, 100, 200, 400 };

#define RAZOR_DEPTH 4
int RAZOR_MARGIN[RAZOR_DEPTH] = {0, 300, 600, 1200};

//-------------------------------------------------------------------------------------------------
//  Search
//-------------------------------------------------------------------------------------------------
int search_zw(GAME *game, UINT incheck, int beta, int depth, UINT can_null, MOVE exclude_move, int prev_move_count)
{
    MOVE_LIST   ml;
    MOVE    trans_move  = MOVE_NONE; 
    int     best_score = -MAX_SCORE;
    int     eval_score = -MAX_SCORE;
    int     move_count = 0;
    int     score = 0;
    UINT    gives_check;
    int     ply = get_ply(&game->board);
    int     turn = side_on_move(&game->board);
    MOVE    move;
    int     alpha;
    int     razor_beta;

    assert(incheck == 0 || incheck == 1);
    assert(beta >= -MAX_SCORE && beta <= MAX_SCORE);
    assert(depth <= MAX_DEPTH);
    assert(can_null == 0 || can_null == 1);

    check_time(game);
    if (game->search.abort) return 0;

    if (depth <= 0) return quiesce(game, incheck, beta - 1, beta, 0);

    game->search.nodes++;
	game->pv_line.pv_size[ply] = ply;

    if (ply > 0 && is_draw(&game->board)) return 0;

    assert(ply >= 0 && ply <= MAX_PLY);
    if (ply >= MAX_PLY) return evaluate(game, beta - 1, beta);

    //  Mate pruning.
    alpha = beta - 1;
    alpha = MAX(-MATE_VALUE + ply, alpha);
    beta = MIN(MATE_VALUE - ply, beta);
    if (alpha >= beta) return alpha;

    // transposition table score or move hint
    if (exclude_move == MOVE_NONE && tt_probe(&game->board, depth, beta - 1, beta, &score, &trans_move)) {
        assert(score >= -MAX_SCORE && score <= MAX_SCORE);
        return score;
    }

    // endgame tablebase probe
#ifdef EGTB_SYZYGY
    U32 tbresult = egtb_probe_wdl(&game->board, depth, ply);

    if (tbresult != TB_RESULT_FAILED) {

        game->search.tbhits++;

        // Convert the WDL value to a score. We consider blessed losses
        // and cursed wins to be a draw, and thus set value to zero.
        //value = tbresult == TB_LOSS ? -MATE + MAX_PLY + height + 1
        //    : tbresult == TB_WIN ? MATE - MAX_PLY - height - 1 : 0;
        score = tbresult == TB_LOSS ? -MATE_VALUE + MAX_PLY + ply + 1 : tbresult == TB_WIN ? +MATE_VALUE - MAX_PLY - ply - 1: 0;

        // Identify the bound based on WDL scores. For wins and losses the
        // bound is not exact because we are dependent on the height, but
        // for draws (and blessed / cursed) we know the tbresult to be exact
        S8 tt_flag = tbresult == TB_LOSS ? TT_UPPER : tbresult == TB_WIN ? TT_LOWER : TT_EXACT;

        if (tt_flag == TT_EXACT || (tt_flag == TT_LOWER && score >= beta) || (tt_flag == TT_UPPER && score <= beta - 1)) {
            tt_save(&game->board, depth, score, tt_flag, MOVE_NONE);
            return score;
        }
    }
#endif 

    // Razoring
    if (exclude_move == MOVE_NONE && !incheck && depth < RAZOR_DEPTH && !is_mate_score(beta)) {
        if (evaluate(game, beta - 1, beta) + RAZOR_MARGIN[depth] < beta && !has_pawn_on_rank7(&game->board, turn)) {
            razor_beta = beta - RAZOR_MARGIN[depth];
            score = quiesce(game, FALSE, razor_beta - 1, razor_beta, 0);
            if (game->search.abort) return 0;
            if (score < razor_beta) {
                return score;
            }
        }
    }

    // Null move: Side to move has advantage that even allowing an extra move to opponent still keeps advantage.
    if (!incheck && can_null && !is_mate_score(beta) && has_pieces(&game->board, turn)) {
        // static null move
        if (depth < STAT_NULL_DEPTH && evaluate(game, beta - 1, beta) - STAT_NULL_MARGIN[depth] >= beta) {
            return evaluate(game, beta - 1, beta) - STAT_NULL_MARGIN[depth];
        }
        
        // null move
        if (depth >= 2 && (depth <= 4 || evaluate(game, beta - 1, beta) >= beta)) {
            make_move(&game->board, pack_null_move());
            score = -search_zw(game, incheck, 1 - beta, null_depth(depth), FALSE, MOVE_NONE, 0);
            undo_move(&game->board);
            if (game->search.abort) return 0;

            if (score >= beta) {
                if (is_mate_score(score)) score = beta;
                tt_save(&game->board, depth, score, TT_LOWER, MOVE_NONE);
                return score;
            }
        }
    }

    select_init(&ml, game, incheck, trans_move, FALSE);
    while ((move = next_move(&ml)) != MOVE_NONE) {

        assert(is_valid(&game->board, move));

        if (!is_pseudo_legal(&game->board, ml.pins, move)) continue;

        move_count++;

        if (move == exclude_move)  continue;

        int reductions = 0;
        int extensions = 0;

        gives_check = is_check(&game->board, move);
        
        // extension if move puts opponent in check
        if (gives_check && (depth < 4 || see_move(&game->board, move) >= 0)) extensions = 1;

        // pruning or depth reductions
        if (!incheck && !extensions && move_count > 1) {
            assert(move != trans_move);

            if (eval_score == -MAX_SCORE) eval_score = evaluate(game, beta - 1, beta);

            // Capture pruning
            if (depth <= 8 && unpack_type(move) == MT_CAPPC && !is_free_pawn(&game->board, turn, move) /*TODO: && unpack_piece(move) != KING*/) {
                if (eval_score + depth * 100 + piece_value(unpack_capture(move)) + 200 < beta) {
                    continue;
                }
            }

            // Quiet moves pruning/reductions
            if (move_is_quiet(move) && !is_killer(&game->move_order, turn, ply, move))  {

                if (!is_counter_move(&game->move_order, flip_color(turn), get_last_move_made(&game->board), move)) {
                    
                    int move_has_bad_history = has_bad_history(&game->move_order, turn, move);
                    
                    // Move count pruning: prune late moves based on move count.
                    if (move_has_bad_history) {
                        int pruning_threshold = 4 + depth * 2;
                        // additional pruning for later moves of previous moves
                        if (depth < 16 && prev_move_count / 8 < pruning_threshold / 2) {
                            pruning_threshold -= prev_move_count / 8;
                        }
                        if (move_count > pruning_threshold) {
                            continue;
                        }
                    }

                    // Futility pruning: eval + margin below beta.
                    if (depth < 10 && eval_score + depth * 100 < beta) {
                        continue;
                    }

                    // Late move reductions: reduce depth for later moves
                    if (move_count > 3 && depth > 2) {
                        reductions = 1;
                        if (depth > 5 && move_has_bad_history) reductions += depth / 6 + move_count / 6;
                        reductions = MIN(reductions, 10);
                    }
                }
            }
        }

        // Make move and search new position.
        make_move(&game->board, move);

        assert(valid_is_legal(&game->board, move));

        score = -search_zw(game, gives_check, 1 - beta, depth - 1 + extensions - reductions, 1, MOVE_NONE, move_count);
        //  Research reduced moves.
        if (!game->search.abort && score >= beta && reductions > 0) {
            score = -search_zw(game, gives_check, 1 - beta, depth - 1, 1, MOVE_NONE, move_count);
        }
        
        undo_move(&game->board);
        if (game->search.abort) return 0;

        // score verification
        if (score > best_score) {
            if (score >= beta) {
                if (exclude_move == MOVE_NONE) {
                    update_pv(&game->pv_line, ply, move);
                    if (move_is_quiet(move)) {
                        move_order_save(&game->move_order, turn, ply, move, &ml, get_last_move_made(&game->board));
                    }
                    tt_save(&game->board, depth, score, TT_LOWER, move);
                }
                return score;
            }
            best_score = score;
        }
    }

    //  Draw or checkmate.
    if (best_score == -MAX_SCORE) {
        if (exclude_move != MOVE_NONE)
            return beta - 1;
        else
            return (incheck ? -MATE_VALUE + ply : 0);
    }

    if (exclude_move == MOVE_NONE) {
        tt_save(&game->board, depth, best_score, TT_UPPER, MOVE_NONE);
    }

    return best_score;
}

// end
