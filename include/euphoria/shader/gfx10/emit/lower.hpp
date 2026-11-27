#pragma once

#include <euphoria/shader/gfx10/emit/bitcode.hpp>
#include <euphoria/shader/gfx10/emit/container.hpp>
#include <euphoria/shader/gfx10/emit/own_bits.hpp>
#include <euphoria/shader/gfx10/emit/shader.hpp>
#include <euphoria/shader/gfx10/emit/wave.hpp>
#include <euphoria/shader/gfx10/error.hpp>
#include <euphoria/shader/gfx10/fields.hpp>
#include <euphoria/shader/gfx10/io.hpp>
#include <euphoria/shader/gfx10/ir.hpp>
#include <euphoria/shader/gfx10/native.hpp>
#include <euphoria/shader/gfx10/resource.hpp>
#include <euphoria/shader/gfx10/structure.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace euphoria::shader::gfx10::emit {

using Why = const char*;

template <class T>
using Res = std::expected<T, Why>;
using Void = std::expected<void, Why>;

struct UserData {
    std::uint32_t index{0};
    std::uint8_t slot{0};
    native::RootType root{};
    std::uint32_t dword{0};

    friend bool operator==(const UserData&, const UserData&) = default;
};

struct Eval {
    enum class Kind : std::uint8_t {
        Mode,
        Weights,
    };

    Kind kind{Kind::Mode};
    std::uint8_t mode{0};
    bool p0{false};
    std::optional<V> i;
    std::optional<V> j;
};

struct Val {
    enum class Kind : std::uint8_t {
        I32,
        Bit,
        Missing,
    };

    Kind kind{Kind::Missing};
    V value{};
    Why why{""};

    static constexpr Val I32(V v) noexcept { return {Kind::I32, v, ""}; }
    static constexpr Val Bit(V v) noexcept { return {Kind::Bit, v, ""}; }
    static constexpr Val Missing(Why w) noexcept { return {Kind::Missing, {}, w}; }
};

struct Lowered {
    std::vector<std::uint8_t> dxil;
    std::uint64_t cmp_samplers{0};
    std::vector<std::uint8_t> root_cbvs;
    std::vector<std::uint8_t> root_srvs;
    std::vector<std::uint8_t> root_uavs;
    std::vector<UserData> user_data;
    std::vector<std::array<std::uint32_t, 4>> samplers;
    std::optional<std::uint32_t> wave_size;
    std::uint32_t nonuniform_handles{0};
    bool wave_ops{false};
};

struct Parts {
    const native::NativeProgram& native;
    const ir::Program& p;
    const structure::Structured& st;
    const io::Io& io;
    const resource::Resources& res;
    std::vector<SigElement> inputs;
    std::vector<SigElement> outputs;
    std::vector<SigElement> prims;
    std::uint32_t mesh_stride{0};
};

struct Stop {
    enum class Kind : std::uint8_t {
        Retry,
        Fail,
    };

    Kind kind{Kind::Fail};
    ir::Value value{};
    Why why{""};
    ShaderError error{};

    static Stop Retry(ir::Value v, Why w) noexcept { return {Kind::Retry, v, w, {}}; }
    static Stop Fail(ShaderError e) noexcept { return {Kind::Fail, {}, "", e}; }
};

inline constexpr float TWO_PI = 6.28318530717958647692f;
inline constexpr std::uint16_t GS_ALLOC_REQ = 9;

std::uint32_t inline_f16(std::uint8_t c) noexcept;

enum class Numeric : std::uint8_t {
    Unorm,
    Snorm,
    Uscaled,
    Sscaled,
    Uint,
    Sint,
    Float,
};

std::optional<std::pair<std::span<const std::uint32_t>, Numeric>> buffer_format(std::uint8_t format) noexcept;
std::uint32_t half_shift(std::uint32_t width) noexcept;

enum class MaskData : std::uint8_t {
    Console,
    Host,
    None,
};

inline constexpr Why LANE_MASK = "a lane mask read as data";
inline constexpr Why MASK_ELSEWHERE = "a lane mask holding lanes elsewhere read as data";
inline constexpr Why FIRST_HALF = "the first half of an interpolation";
inline constexpr std::uint32_t LOOP_LIMIT = 1u << 20;

std::optional<std::vector<bool>> all_lane_blocks(const structure::Structured& st);
std::optional<std::vector<std::pair<std::size_t, std::vector<bool>>>> lane_regions(const structure::Structured& st);
std::optional<std::vector<std::size_t>> branches_around(const structure::Structured& st,
                                                        std::span<const std::size_t> needs);
bool needs_every_lane(std::string_view what);
std::vector<std::size_t> take_needs();
std::unordered_set<ir::Value> lane_uniform(const ir::Program& p, const structure::Structured& st,
                                           std::span<const bool> all);

enum class Wave : std::uint8_t {
    Plain,
    First,
    Total,
    Prefix,
    Base,
    Mixed,
};

std::expected<void, std::pair<std::size_t, Why>> wave_local(const ir::Program& p,
                                                            const structure::Structured& st);
std::uint32_t meeting_mode() noexcept;
std::optional<std::vector<std::pair<bool, bool>>> lds_meetings(const ir::Program& p,
                                                               const structure::Structured& st,
                                                               std::span<const bool> all);
bool lane_constant_masks(const structure::Structured& st);
void set_host_max_wave(std::uint32_t lanes);
bool f16_nearest_on();
bool own_bits_on();
bool first_half_pairs();
void set_first_half_pairs(bool on);
template <class F>
decltype(auto) with_first_half_pairs(F&& f)
{
    struct Restore {
        bool previous;
        ~Restore() { set_first_half_pairs(previous); }
    } restore{first_half_pairs()};
    set_first_half_pairs(true);
    return std::forward<F>(f)();
}
std::uint32_t host_max_wave();
bool reads_mask_as_data(const structure::Structured& st, const ir::Inst& inst);

using PackedLayout = std::vector<std::pair<std::uint8_t, std::vector<std::tuple<std::uint8_t, std::uint8_t, std::uint8_t, std::uint8_t>>>>;

PackedLayout packed_layout(std::span<const SigElement> elements,
                           std::span<const std::pair<std::uint8_t, std::optional<std::uint8_t>>> packs);
std::optional<V> packed_half(const PackedLayout& layout, std::uint8_t row, std::uint8_t col,
                             bool high);
std::vector<std::pair<std::uint8_t, std::optional<std::uint8_t>>> input_packs(
    const native::StageIo& io, std::span<const SigElement> inputs);
std::map<std::uint8_t, std::pair<bool, std::optional<std::uint8_t>>> half_outputs(
    const native::StageIo& io, std::span<const SigElement> outputs);
std::map<std::uint8_t, std::uint8_t> half_input_partners(const native::StageIo& io,
                                                         std::span<const SigElement> inputs);
Stop unsupported(std::size_t offset, Why what);
std::optional<std::uint8_t> interp_mode(io::Barycentric kind);

class Lower {
public:
    Lower(const Parts& x, const std::unordered_map<ir::Value, Why>& bad);

    Shader s;
    const Parts& x;
    const std::unordered_map<ir::Value, Why>& bad;
    std::unordered_map<ir::Value, Why> lane_fail;
    std::unordered_map<ir::Value, Val> vals;
    std::unordered_map<ir::Value, V> floats;
    std::vector<std::optional<std::size_t>> block;
    std::size_t cur{0};
    std::map<std::pair<std::size_t, structure::Bool>, V> bools;
    std::unordered_map<ir::Value, V> lane_bits;
    std::unordered_map<ir::Value, std::pair<std::size_t, std::size_t>> phi_at;
    std::unordered_set<ir::Value> stale;
    std::unordered_map<ir::Value, V> mask_phis;
    std::vector<std::tuple<V, std::size_t, std::size_t>> pending_masks;
    std::vector<std::tuple<V, std::size_t, std::size_t>> data_phis;
    std::unordered_set<ir::Value> read_phis;
    std::unordered_map<ir::Value, std::pair<std::size_t, std::uint32_t>> params;
    std::optional<V> user_data;
    std::map<std::uint32_t, UserData> user_reads;
    std::vector<std::array<std::uint32_t, 4>> samplers;
    std::map<std::pair<resource::Site, bool>, const resource::Source*> descriptors;
    std::map<resource::Site, resource::Address> scalar;
    std::map<resource::Site, const io::Export*> exports;
    std::map<resource::Site, const io::Interp*> interps;
    std::map<std::tuple<std::size_t, resource::Site, std::uint8_t, bool, std::pair<std::uint32_t, std::uint32_t>>, V> handles;
    std::map<std::tuple<std::size_t, resource::Site, std::uint8_t>, V> strides;
    std::map<std::pair<std::size_t, std::size_t>, std::size_t> edges;
    std::vector<bool> exported;
    std::vector<std::size_t> rpo;
    std::vector<std::optional<std::size_t>> tail;
    bool writes_uavs{false};
    std::optional<V> src0;
    bool console_lanes{false};
    std::optional<V> flat;
    std::unordered_set<std::size_t> no_barrier;
    std::unordered_set<std::size_t> fence_only;
    bool split_lds{false};
    std::optional<std::pair<std::vector<bool>, std::vector<std::pair<bool, bool>>>> lds_meet;
    std::optional<std::tuple<std::uint32_t, bool, std::vector<bool>>> exchange;
    bool local{false};
    bool local_ok{false};
    bool local_any{false};
    std::unordered_set<ir::Value> uniform;
    own_bits::OwnBits own_bits;
    std::optional<V> clane;
    std::optional<V> pair;
    std::optional<std::uint32_t> halves;
    std::optional<V> wave_base_value;
    std::map<std::uint8_t, std::pair<bool, std::optional<std::uint8_t>>> half_outputs;
    MaskData mask_data{MaskData::None};
    bool mask_ok{false};
    std::unordered_map<ir::Value, std::pair<V, V>> ballots;
    std::unordered_set<ir::Value> balloting;
    std::unordered_map<ir::Value, std::size_t> def_block;
    std::size_t cur_ir{0};
    std::map<std::size_t, std::map<std::size_t, wave::Section>> sections;
    std::unordered_set<ir::Value> section_out;
    std::optional<std::tuple<V, std::uint32_t, std::uint32_t>> code_table;
    std::map<std::size_t, std::pair<V, std::vector<std::pair<V, std::size_t>>>> counters;
    std::optional<std::size_t> abort;

    std::expected<Lowered, Stop> run();

    V code_dword(V byte);
    Res<void> flat_inst(const ir::Inst& inst, std::size_t b, std::size_t i);
    void find_sections();
    void section_inst(std::size_t b, std::size_t i, std::size_t start);
    Res<V> shape(wave::Shape shape, bool own);
    Res<V> active_f64_sum(ir::Value lo, ir::Value hi, ir::Value mask, bool high);
    V add_halves(V a, V b);
    Res<V> half_sum(V x);
    V half_sum_present(V x);

    bool exported_out(std::size_t b) const;
    bool has_done_export(std::size_t b) const;
    std::expected<void, Stop> inputs();
    Val input(const io::Input& input);
    Val compute_id(std::string_view name, std::uint32_t opcode, std::uint8_t c);
    std::optional<std::pair<std::size_t, std::uint8_t>> find_input(std::uint8_t row,
                                                                   std::uint8_t channel) const;
    Val load_input(std::size_t id, std::uint8_t col);
    Val load_any_input(std::size_t id, std::uint8_t col);
    Res<Val> get(ir::Value v);
    std::optional<Res<void>> lazy_barycentric(ir::Value v);
    Val barycentric(io::Barycentric kind, std::uint8_t index);
    Res<V> i32_of(ir::Value v);
    Val user(std::size_t param, std::uint32_t dword);
    std::uint32_t user_first() const;
    V host_word(native::RootType root, std::uint8_t slot, std::uint32_t dword);
    V user_at(std::uint32_t index);
    bool computed_with(ir::Value v, std::uint32_t depth) const;
    std::optional<std::pair<V, V>> view_address(resource::Site site);
    std::pair<V, V> address_plus(V lo, V hi, std::int32_t off);
    std::pair<V, V> memory(V lo, V hi, std::optional<V> guard);
    Res<void> memory_flat(const ir::Inst& inst, std::size_t b, std::size_t i);
    Res<void> memory_smem(const ir::Inst& inst);
    Res<V> mask_dword(ir::Value v);
    Res<V> operand(const ir::Inst& inst, const Operand& op);
    Res<V> f16_operand(const ir::Inst& inst, const Operand& op);
    V as_float(V x);
    V as_int(V f);
    static std::optional<std::pair<std::uint8_t, bool>> sdwa_src(const ir::Inst& inst, std::size_t k);
    V select_bits(V x, std::uint8_t sel, bool sext);
    Res<V> fsrc(const ir::Inst& inst, std::size_t k);
    Res<V> src(const ir::Inst& inst, std::size_t k);
    V unary(std::uint32_t op, V f);
    V binary(std::uint32_t op, V a, V b);
    V max_min(bool max, V a, V b);
    V fmad(V a, V b, V c);
    V f16_toward_zero(V f);
    V f16_nearest(V f);
    V bfe(V a, V off, V width, bool signed_);
    V is_nan_f32(V f);
    V med3(V a, V b, V third);
    V fract(V a, float threshold, float below_one);
    V trig_turns(V x, bool cosine);
    V mul_hi(V a, V b, bool signed_);
    V legacy_mul(V a, V b);
    Res<V> boolean(structure::Bool b, std::size_t at);
    Res<V> atom(structure::Atom atom, std::size_t at);
    std::optional<V> wave_base();
    V console_thread();
    V console_lane();
    void group_sync(std::size_t block);
    std::pair<V, V> exchange_halves(std::size_t at, V mine);
    V exchange_pair(std::uint32_t slot);
    Res<V> scc_some(ir::Value scc);
    Res<V> wave_wide(std::size_t b, structure::Bool when);
    bool all_lanes(std::size_t b) const;
    V lane_bit(std::size_t at, V lo, std::optional<V> hi);
    std::optional<ir::Value> pair_high(ir::Value lo) const;
    Res<std::optional<V>> guard(std::size_t b, std::size_t i);
    void set(std::optional<ir::Value> v, Val val);
    void write(const ir::Inst& inst, ir::Reg reg, V x, std::optional<V> guard);
    void write_placed(const ir::Inst& inst, ir::Reg reg, V x, std::optional<V> guard);
    static std::optional<std::pair<std::uint8_t, std::uint8_t>> sdwa_dst(const ir::Inst& inst);
    V place_bits(const ir::Inst& inst, ir::Reg reg, V x, std::uint8_t sel, std::uint8_t unused);
    Res<void> write_f(const ir::Inst& inst, V f, std::optional<V> guard);
    Res<void> write_i(const ir::Inst& inst, V x, std::optional<V> guard);
    std::optional<std::pair<std::size_t, std::size_t>> begin_guarded(std::optional<V> guard);
    std::vector<V> end_guarded(std::optional<std::pair<std::size_t, std::size_t>> at,
                               std::span<const std::pair<V, V>> values);
    Res<void> cross_lane(bool quad);
    Val thread_id(std::uint8_t c);
    Res<void> mesh_prologue(Mesh mesh);
    V mesh_thread();
    std::uint32_t wave() const;
    V lane_index();
    V quad_pick(V x, V src);
    std::pair<V, V> first_lane(V x, std::optional<V> guard);
    V read_lane(V x, V lane);
    Res<V> float_of(ir::Value v);
    Res<std::vector<V>> vgprs(const ir::Inst& inst, const Operand& op, std::uint8_t n);
    Res<void> ds(const ir::Inst& inst, std::size_t b, std::size_t i);
    V lds_float_min_max(V p, V data, bool max);
    V lds_float_pick(V mem, V data, bool max);
    std::expected<void, Stop> inst(std::size_t b, std::size_t i);
    void lds_meeting();
    bool dead(ir::Value v, std::vector<ir::Value>& seen) const;
    bool uniform_test(const ir::Inst& inst) const;
    bool is_mask(std::optional<ir::Value> v) const;
    Res<void> sop(const ir::Inst& inst);
    std::pair<V, V> add_sub_carry(std::size_t c, bool add, V a, V b, std::optional<V> cin);
    Res<V> scc_in(const ir::Inst& inst);
    static bool wide_loads();
    std::vector<V> raw_dwords(V h, V addr, std::uint32_t count);
    Res<void> smem(const ir::Inst& inst, resource::Site site);
    Res<void> descriptor_data(const ir::Inst& inst, resource::Site site);
    std::optional<std::tuple<std::uint8_t, std::pair<std::uint32_t, std::uint32_t>,
                             std::optional<std::uint32_t>, bool, const native::UsedResource*>>
    unused_descriptor_kind(const ir::Inst& inst, resource::Site site, std::uint8_t code,
                           std::uint32_t count) const;
    Res<void> code_smem(const ir::Inst& inst, resource::Address a);
    V cbuffer_dword(V h, V a);
    Res<std::vector<V>> format_load(V h, V off, std::uint8_t format, std::uint32_t channels);
    Res<std::pair<std::uint32_t, std::optional<V>>> built_offset(const resource::Source& src);
    std::optional<V> root_bias(std::size_t param);
    const native::UsedResource* used_kind(const resource::Source& src) const;
    Res<V> handle(resource::Site site, const resource::Source& src, bool sampler,
                  std::pair<std::uint32_t, std::uint32_t> props);
    Res<V> descriptor_phi(std::size_t block, std::span<const resource::Source> args, bool sampler,
                          std::uint32_t depth);
    Res<std::optional<V>> slot_stride(const resource::Source& src);
    Res<V> lane_heap_index(const resource::Source& src, bool sampler);
    Res<V> fetched_index(resource::Site load, std::uint8_t first, bool sampler);
    Res<V> heap_index(resource::Site load, std::uint8_t first, bool sampler);
    Res<void> valu(const ir::Inst& inst, std::size_t b, std::size_t i);
    Res<std::pair<V, std::optional<V>>> dpp(const ir::Inst& inst, Dpp dpp);
    Res<void> valu_op(const ir::Inst& inst, std::optional<V> guard);
    Res<V> src64(const ir::Inst& inst, std::size_t k);
    Res<std::pair<V, V>> f64_pair(const ir::Inst& inst, std::size_t k);
    V f64_to_f32_bits(V lo, V hi);
    V join64(V lo, V hi);
    V f64_bits_to_f32_bits(V lo, V hi);
    std::pair<V, V> split64(V x);
    Res<V> half_src(const ir::Inst& inst, std::size_t k);
    Res<V> half_fsrc(const ir::Inst& inst, std::size_t k);
    Res<void> write_half(const ir::Inst& inst, V x, std::optional<V> guard);
    std::optional<Res<void>> valu16(const ir::Inst& inst, std::string_view name,
                                    std::optional<V> guard);
    Res<void> valu16_op(const ir::Inst& inst, std::string_view name, std::optional<V> guard,
                        std::span<const std::string_view> float1,
                        std::span<const std::string_view> float2,
                        std::span<const std::string_view> float3);
    Res<V> cvt_norm16(const ir::Inst& inst, bool signed_);
    Res<V> packed_src(const ir::Inst& inst, std::size_t k, bool high, bool float_);
    Res<void> packed16(const ir::Inst& inst, std::string_view op, std::optional<V> guard);
    Res<V> integer(const ir::Inst& inst, std::string_view name);
    Res<void> compare(const ir::Inst& inst, std::string_view rest);
    void lane(const ir::Inst& inst, std::optional<bool> value);
    void lane_bits_for(const ir::Inst& inst, V r);
    Res<void> interp(const ir::Inst& inst, resource::Site site);
    bool per_vertex(std::uint8_t row) const;
    bool interpolated(std::uint8_t row) const;
    Res<V> eval_channel(std::uint8_t row, std::uint8_t ch, Eval eval);
    V vertex_input(std::size_t id, std::uint8_t col, std::uint8_t k);
    V low_half(const ir::Inst& inst, std::uint8_t reg, V x);
    Res<void> image(const ir::Inst& inst, std::size_t b, std::size_t i);
    std::array<V, 3> cube_direction(V s, V t, V face);
    Res<void> buffer(const ir::Inst& inst, std::size_t b, std::size_t i);
    Res<void> export_(std::size_t b, std::size_t i);
    Res<void> emit_indices(const io::Export& e, std::optional<V> guard);
    std::optional<std::uint8_t> unnamed_row() const;
    std::expected<void, Stop> exit(std::size_t b);
    void enter_loop(std::size_t from, std::size_t to, std::size_t via);
    V count_iteration(std::size_t to, std::uint8_t p);
    std::size_t abort_block();
    std::size_t edge(std::size_t from, std::size_t to);
    std::expected<void, Stop> fill_phis();
};

std::expected<Lowered, ShaderError> lower(const Parts& parts);

}
