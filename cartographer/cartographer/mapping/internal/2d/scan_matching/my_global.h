#ifndef __MY_GLOBAL_H
#define __MY_GLOBAL_H
namespace cartographer{
namespace mapping{
    extern bool have_done_match_full_submap;
    extern int my_max_constraint_distance;//cartographer/mapping/internal/2d/scan_matching/pose_graph_2d.cc
    extern bool now_is_border;//cartographer/mapping/internal/2d/scan_matching/pose_graph_2d.cc
}
}

#endif
