#include "ccv.h"
#include "assert.h"

uint64_t extract(const GrB_Matrix A, const GrB_Index i, const GrB_Index j) {
    uint64_t x;
    GrB_Info info = ok(GrB_Matrix_extractElement_UINT64(&x, A, i, j), false);
    if (info == GrB_NO_VALUE) {
        return 0xcccccccccccccccc;
    }
    return x;
}

uint64_t extract_v(const GrB_Vector v, const GrB_Index i) {
    uint64_t x;
    GrB_Info info = ok(GrB_Vector_extractElement_UINT64(&x, v, i), false);
    if (info == GrB_NO_VALUE) {
        return 9999999;
    }
    return x;
}

void print_bit_matrix(const GrB_Matrix A) {
    GrB_Index nrows, ncols;
    ok(GrB_Matrix_nrows(&nrows, A));
    ok(GrB_Matrix_ncols(&ncols, A));
    for (GrB_Index i = 0; i < nrows; i++) {
        printf("%4ld:", i);
        for (GrB_Index j = 0; j < ncols; j++) {
            printf(" %016lx", extract(A, i, j));
        }
        printf("\n");
    }
    printf("\n");
}

void print_bit_matrices(const GrB_Matrix frontier, const GrB_Matrix next, const GrB_Matrix seen,
                        const GrB_Vector next_popcount, const GrB_Vector sp) {
    GrB_Index nrows, ncols;
    ok(GrB_Matrix_nrows(&nrows, frontier));
    ok(GrB_Matrix_ncols(&ncols, frontier));
    printf("          frontier             next               seen         next_popcount    sp\n");
    for (GrB_Index i = 0; i < nrows; i++) {
        printf("%4ld:", i);
        for (GrB_Index j = 0; j < ncols; j++) {
            printf(
                    " %016lx   %016lx   %016lx   %13ld   %3ld",
                    extract(frontier, i, j), extract(next, i, j), extract(seen, i, j), extract_v(next_popcount, i),
                    extract_v(sp, i));
        }
        printf("\n");
    }
    printf("\n");
}

inline __attribute__((always_inline))
void create_diagonal_bit_matrix(GrB_Matrix D) {
    GrB_Index n, nrows;
    ok(GrB_Matrix_ncols(&n, D));
#ifndef NDEBUG
    ok(GrB_Matrix_nrows(&nrows, D));
    assert(nrows == (n + 63) / 64);
#endif

//    I = 0, 1, ..., n
//    J = 0, 0, ..., 0 [64], 1, 1, ..., 1 [64], ..., ceil(n/64)
//    X = repeat {b100..., b010..., b001..., ..., b...001} until we have n elements
    std::unique_ptr<GrB_Index[]> I{new GrB_Index[n]}, J{new GrB_Index[n]};
    std::unique_ptr<uint64_t[]> X{new uint64_t[n]};

    int nthreads = GlobalNThreads;
    nthreads = std::min<size_t>(n / 4096, nthreads);
    nthreads = std::max(nthreads, 1);
#pragma omp parallel for num_threads(nthreads) schedule(static)
    for (GrB_Index k = 0; k < n; k++) {
        I[k] = k / 64;
        J[k] = k;
        X[k] = 1L << (k % 64);
    }
    ok(GrB_Matrix_build_UINT64(D, I.get(), J.get(), X.get(), n, GrB_BOR_UINT64));
}

void fun_sum_popcount(void *z, const void *x) {
    *((uint64_t *) z) = __builtin_popcountll(*((uint64_t *) x));
}

// TODO: mapping comes from LAGraph (C code) and needs to be freed
std::tuple<GBxx_Object<GrB_Vector>, std::unique_ptr<GrB_Index[]>> compute_ccv(GrB_Matrix A) {
    // initializing unary operator for next_popcount
    GBxx_Object<GrB_UnaryOp> op_popcount = GB(GrB_UnaryOp_new, fun_sum_popcount, GrB_UINT64, GrB_UINT64);
    GBxx_Object<GrB_Semiring> BOR_FIRST, BOR_SECOND;
    BOR_FIRST = GB(GrB_Semiring_new, GxB_BOR_UINT64_MONOID, GrB_FIRST_UINT64);
    BOR_SECOND = GB(GrB_Semiring_new, GxB_BOR_UINT64_MONOID, GrB_SECOND_UINT64);

    GrB_Index n;
    ok(GrB_Matrix_nrows(&n, A));
    {
        GrB_Index ncols;
        ok(GrB_Matrix_ncols(&ncols, A));
        assert(n == ncols); // TODO replace with proper input check
    }

    const GrB_Index bit_matrix_ncols = (n + 63) / 64;

    GBxx_Object<GrB_Matrix> frontier = GB(GrB_Matrix_new, GrB_UINT64, bit_matrix_ncols, n);
    GBxx_Object<GrB_Matrix> next = GB(GrB_Matrix_new, GrB_UINT64, bit_matrix_ncols, n);
    GBxx_Object<GrB_Matrix> Next_PopCount = GB(GrB_Matrix_new, GrB_UINT64, bit_matrix_ncols, n);
    GBxx_Object<GrB_Matrix> Seen_PopCount = GB(GrB_Matrix_new, GrB_UINT64, bit_matrix_ncols, n);

    GBxx_Object<GrB_Vector> next_popcount = GB(GrB_Vector_new, GrB_UINT64, n);
    GBxx_Object<GrB_Vector> ones = GB(GrB_Vector_new, GrB_UINT64, n);
    GBxx_Object<GrB_Vector> n_minus_one = GB(GrB_Vector_new, GrB_UINT64, n);
    GBxx_Object<GrB_Vector> level_v = GB(GrB_Vector_new, GrB_UINT64, n);
    GBxx_Object<GrB_Vector> sp = GB(GrB_Vector_new, GrB_UINT64, n);
    GBxx_Object<GrB_Vector> compsize = GB(GrB_Vector_new, GrB_UINT64, n);
    GBxx_Object<GrB_Vector> ccv_result = GB(GrB_Vector_new, GrB_FP64, n);

    // initialize frontier and seen matrices: to compute closeness centrality, start off with a diagonal
    create_diagonal_bit_matrix(frontier.get());
    GBxx_Object<GrB_Matrix> seen = GB(GrB_Matrix_dup, frontier.get());

    // initialize vectors
    ok(GrB_Vector_assign_UINT64(ones.get(), NULL, NULL, 1, GrB_ALL, n, NULL));
    ok(GrB_Vector_assign_UINT64(n_minus_one.get(), NULL, NULL, n - 1, GrB_ALL, n, NULL));

    // initialize


    bool push = true; // TODO: add heuristic
    //Heuristic
    float threshold = 0.015;
    GrB_Index frontier_nvals, matrix_nrows, source_nrows;
    ok(GrB_Matrix_nvals(&frontier_nvals, frontier.get()));
    ok(GrB_Matrix_nrows(&matrix_nrows, A));
    //This should be the amount of bfs traversals, but since we traverse from all
    //nodes, this equals A.nrows
    ok(GrB_Matrix_nrows(&source_nrows, A));
    float r, r_before;
    r_before = (float) frontier_nvals / (float) (matrix_nrows * source_nrows);

    GrB_Matrix C = NULL;
    GrB_Index* mapping = NULL;
    LAGraph_reorder_vertices(&C, &mapping, A, false);
    A = C;


    // traversal
    for (GrB_Index level = 1; level < n; level++) {
//        printf("========================= Level %2ld =========================\n\n", level);
        // level_v += 1
        ok(GrB_Vector_eWiseAdd_BinaryOp(level_v.get(), NULL, NULL, GrB_PLUS_UINT64, level_v.get(), ones.get(), NULL));

        // next = A * frontier
        ok(GrB_mxm(next.get(), NULL, NULL, BOR_FIRST.get(), frontier.get(), A, GrB_DESC_R));
//        ok(GrB_mxm(next.get(), NULL, NULL, BOR_SECOND.get(), A, frontier.get(), NULL));

        // next = next & ~seen
        // We need to use eWiseAdd to see the union of value but mask with next so that
        // zero elements do not get the value from ~seen.
        // The code previously dropped zero elements. DO NOT do that as it will render
        // seen[i] = 0000 (implicit value) and seen[j] = 1111 equivalent in not_seen
        /*
             (Next)&& ! Seen  =  Next
              1100      1010     0100
                 -      0001        -
              1111         -     1111
            ----------------------------
             (Next)&&(neg(Seen)) = Next
              1100      0101       0100
                 -      1110          -
              1111         -       1111
            ----------------------------
            neg: apply f(a)=~a on explicit values
            GrB_apply: C<Mask> = accum (C, op(A))
            mask: Next
            desc: default (do not replace) to keep values which don't have corresponding seen values
            accum: &
            op: ~
            Next<Next> &= ~Seen
            (Seen = Next)
         */

        ok(GrB_Matrix_apply(next.get(), next.get(), GrB_BAND_UINT64, GrB_BNOT_UINT64, seen.get(), NULL));
        ok(GxB_Matrix_select(next.get(), GrB_NULL, GrB_NULL, GxB_NONZERO, next.get(), GrB_NULL, GrB_NULL));
        GrB_Index next_nvals;
        ok(GrB_Matrix_nvals(&next_nvals, next.get()));


        if (next_nvals == 0) {
//            printf("no new vertices found\n");
            break;
        }
        // next_popCount = reduce(apply(popcount, next))
        ok(GrB_Matrix_apply(Next_PopCount.get(), NULL, NULL, op_popcount.get(), next.get(), NULL));
        ok(GrB_Matrix_reduce_Monoid(next_popcount.get(), NULL, NULL, GxB_PLUS_UINT64_MONOID, Next_PopCount.get(),
                GrB_DESC_T0));

        // seen = seen | next
        ok(GrB_Matrix_eWiseAdd_BinaryOp(seen.get(), NULL, NULL, GrB_BOR_UINT64, seen.get(), next.get(), NULL));

        // sp += (next_popcount * level)
        //   next_popcount * level is expressed as next_popcount *= level_v
        ok(GxB_Vector_subassign_UINT64(level_v.get(), next_popcount.get(), NULL, level, GrB_ALL, n, GrB_DESC_S));
        ok(GrB_Vector_eWiseMult_BinaryOp(next_popcount.get(), NULL, NULL, GrB_TIMES_UINT64, next_popcount.get(),
                                         level_v.get(), NULL));
        ok(GrB_Vector_eWiseAdd_BinaryOp(sp.get(), NULL, NULL, GrB_PLUS_UINT64, sp.get(), next_popcount.get(), NULL));

//        print_bit_matrices(frontier, next, seen, next_popcount, sp);

        // frontier = next
        frontier = GB(GrB_Matrix_dup, next.get());

        //Heuristic
        ok(GrB_Matrix_nvals(&frontier_nvals, frontier.get()));
        r = (float) frontier_nvals / (float) (matrix_nrows * source_nrows);

        if (r > r_before && r > threshold) {
            push = false;
        }

        if (r < r_before && r < threshold) {
            push = true;
        }

        r_before = r;
    }
    // compsize = reduce(seen, row -> popcount(row))
    ok(GrB_Matrix_apply(Seen_PopCount.get(), NULL, NULL, op_popcount.get(), seen.get(), NULL));
    ok(GrB_Matrix_reduce_Monoid(compsize.get(), NULL, NULL, GxB_PLUS_UINT64_MONOID, Seen_PopCount.get(), GrB_DESC_T0));

    // compute the closeness centrality value:
    //
    //          (C(p)-1)^2
    // CCV(p) = ----------
    //          (n-1)*s(p)

    // all vectors are dense therefore eWiseAdd and eWiseMult are the same
    // C(p)-1
    ok(GrB_Vector_eWiseAdd_BinaryOp(compsize.get(), NULL, NULL, GrB_MINUS_UINT64, compsize.get(), ones.get(), NULL));
    // (C(p)-1)^2
    ok(GrB_Vector_eWiseMult_BinaryOp(compsize.get(), NULL, NULL, GrB_TIMES_UINT64, compsize.get(), compsize.get(),
                                     NULL));

    ok(GrB_Vector_eWiseMult_BinaryOp(sp.get(), NULL, NULL, GrB_TIMES_UINT64, n_minus_one.get(), sp.get(), NULL));
    ok(GrB_Vector_eWiseMult_BinaryOp(ccv_result.get(), NULL, NULL, GrB_DIV_FP64, compsize.get(), sp.get(), NULL));

//    ok(GxB_print(compsize, GxB_SHORT));
//    ok(GxB_print(sp, GxB_SHORT));

    return std::tuple<GBxx_Object<GrB_Vector>, std::unique_ptr<GrB_Index[]>>{std::move(ccv_result), mapping};
}
