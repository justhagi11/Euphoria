#include <euphoria/shader/gfx10/emit/shader.hpp>

#include <euphoria/shader/gfx10/emit/bitcode_census.hpp>
#include <euphoria/shader/gfx10/emit/bitcode_opt.hpp>

#include <algorithm>
#include <atomic>
#include <format>
#include <limits>
#include <string>

namespace euphoria::shader::gfx10::emit {
namespace {

std::atomic<std::uint32_t> g_view_limit{0};

constexpr std::uint32_t kMax32 = std::numeric_limits<std::uint32_t>::max();

std::uint32_t next_multiple_of(std::uint32_t v, std::uint32_t m) noexcept
{
    return (v + m - 1) / m * m;
}

void put32(std::vector<std::uint8_t>& out, std::uint32_t v)
{
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    }
}

std::uint32_t scalars(const std::vector<SigElement>& els)
{
    std::uint32_t best = 0;
    for (const SigElement& e : els) {
        if (e.row) {
            best = std::max(best, *e.row * 4 + static_cast<std::uint32_t>(e.col + e.cols));
        }
    }
    return best;
}

int binding_rank(std::uint32_t kind) noexcept
{
    switch (kind) {
    case 2:
        return 0;
    case 1:
        return 1;
    case 7:
        return 3;
    default:
        return 2;
    }
}

}

void set_view_heap_limit(std::uint32_t null_slot)
{
    g_view_limit.store(null_slot, std::memory_order_relaxed);
}

std::uint32_t view_heap_limit()
{
    return g_view_limit.load(std::memory_order_relaxed);
}

Shader::Shader(bool pixel_shader)
    : pixel(pixel_shader)
{
    const Ty void_t = m.void_();
    const Ty i1 = m.int_(1);
    const Ty i8 = m.int_(8);
    const Ty i32t = m.int_(32);
    const Ty f32t = m.float_();
    const Ty i8p = m.ptr(i8);
    const Ty handle = m.named_struct("dx.types.Handle", {i8p});
    const Ty props = m.named_struct("dx.types.ResourceProperties", {i32t, i32t});
    const Ty bind = m.named_struct("dx.types.ResBind", {i32t, i32t, i32t, i8});
    const Ty cbuf_ret = m.named_struct("dx.types.CBufRet.i32", {i32t, i32t, i32t, i32t});
    const Ty res_ret = m.named_struct("dx.types.ResRet.f32", {f32t, f32t, f32t, f32t, i32t});
    const Ty main_ty = m.func_ty(void_t, {});
    m.ptr(main_ty);
    main_ = m.function("main", main_ty, std::nullopt, true);
    entry = m.new_block();
    t = Types{void_t, i1, i32t, f32t, handle, props, bind, cbuf_ret, res_ret};
}

V Shader::lds()
{
    if (lds_global_) {
        return *lds_global_;
    }
    const Ty arr = m.array(lds_dwords, t.i32);
    const V g = m.global("lds", arr, 3);
    lds_global_ = g;
    return g;
}

V Shader::op(std::size_t block, std::string_view name, Attr attr, Ty ret, std::uint32_t opcode, std::span<const V> args)
{
    std::vector<Ty> params{t.i32};
    params.reserve(args.size() + 1);
    for (const V a : args) {
        params.push_back(m.type_of(a));
    }
    const Ty fty = m.func_ty(ret, std::move(params));
    m.ptr(fty);
    const V f = m.function(std::format("dx.op.{}", name), fty, attr, false);
    std::vector<V> all{m.i32(opcode)};
    all.insert(all.end(), args.begin(), args.end());
    return m.call(block, f, std::move(all));
}

V Shader::op(std::size_t block, std::string_view name, Attr attr, Ty ret, std::uint32_t opcode, std::initializer_list<V> args)
{
    return op(block, name, attr, ret, opcode, std::span<const V>{args.begin(), args.size()});
}

V Shader::user_data_handle()
{
    const V b0 = m.i32(0);
    const V b1 = m.i32(0);
    const V b2 = m.i32(USER_DATA_SPACE);
    const V b3 = m.i8(2);
    const V bind = m.aggregate(t.bind, {b0, b1, b2, b3});
    const std::size_t block = entry;
    const V i = m.i32(0);
    const V f = m.bool_(false);
    const V h = op(block, "createHandleFromBinding", Attr::ReadNone, t.handle, 217, {bind, i, f});
    return annotate(block, h, 13, kMax32);
}

V Shader::root_cbv_handle(std::uint8_t slot)
{
    for (const auto& [s, h] : root_cbvs) {
        if (s == slot) {
            return h;
        }
    }
    const V r = m.i32(slot);
    const V space = m.i32(ROOT_CBV_SPACE);
    const V kind = m.i8(2);
    const V bind = m.aggregate(t.bind, {r, r, space, kind});
    const V f = m.bool_(false);
    const std::size_t block = entry;
    V h = op(block, "createHandleFromBinding", Attr::ReadNone, t.handle, 217, {bind, r, f});
    h = annotate(block, h, 13, 65536);
    root_cbvs.emplace_back(slot, h);
    return h;
}

V Shader::root_srv_handle(std::uint8_t slot)
{
    for (const auto& [s, h] : root_srvs) {
        if (s == slot) {
            return h;
        }
    }
    const V r = m.i32(slot);
    const V space = m.i32(ROOT_CBV_SPACE);
    const V kind = m.i8(0);
    const V bind = m.aggregate(t.bind, {r, r, space, kind});
    const V f = m.bool_(false);
    const std::size_t block = entry;
    V h = op(block, "createHandleFromBinding", Attr::ReadNone, t.handle, 217, {bind, r, f});
    h = annotate(block, h, 11, 0);
    flags |= flags::RAW_BUFFERS;
    root_srvs.emplace_back(slot, h);
    return h;
}

V Shader::root_uav_handle(std::uint8_t slot)
{
    for (const auto& [s, h] : root_uavs) {
        if (s == slot) {
            return h;
        }
    }
    const V r = m.i32(slot);
    const V space = m.i32(ROOT_CBV_SPACE);
    const V kind = m.i8(1);
    const V bind = m.aggregate(t.bind, {r, r, space, kind});
    const V f = m.bool_(false);
    const std::size_t block = entry;
    V h = op(block, "createHandleFromBinding", Attr::ReadNone, t.handle, 217, {bind, r, f});
    h = annotate(block, h, 11 | 0x1000, 0);
    flags |= flags::RAW_BUFFERS;
    root_uavs.emplace_back(slot, h);
    return h;
}

V Shader::sampler_handle(std::size_t block, std::uint32_t n)
{
    const V b0 = m.i32(n);
    const V b1 = m.i32(n);
    const V b2 = m.i32(SAMPLER_SPACE);
    const V b3 = m.i8(3);
    const V bind = m.aggregate(t.bind, {b0, b1, b2, b3});
    const V i = m.i32(n);
    const V f = m.bool_(false);
    const V h = op(block, "createHandleFromBinding", Attr::ReadNone, t.handle, 217, {bind, i, f});
    const std::uint32_t cmp = ((cmp_samplers >> n) & 1) != 0 ? 0x8000u : 0u;
    return annotate(block, h, 14 | cmp, 0);
}

V Shader::annotate(std::size_t block, V h, std::uint32_t props0, std::uint32_t props1)
{
    const V p0 = m.i32(props0);
    const V p1 = m.i32(props1);
    const V p = m.aggregate(t.props, {p0, p1});
    return op(block, "annotateHandle", Attr::ReadNone, t.handle, 216, {h, p});
}

V Shader::heap_handle(std::size_t block, V index, bool sampler, bool nonuniform, std::pair<std::uint32_t, std::uint32_t> props)
{
    const std::uint32_t limit = g_view_limit.load(std::memory_order_relaxed);
    V clamped = index;
    if (!sampler && limit != 0) {
        const V null = m.i32(limit);
        clamped = op(block, "binary.i32", Attr::ReadNone, t.i32, 40, {index, null});
        m.tag(clamped, Tag::Clamp);
    } else if (sampler) {
        const V last = m.i32(2047);
        clamped = op(block, "binary.i32", Attr::ReadNone, t.i32, 40, {index, last});
        m.tag(clamped, Tag::Clamp);
    }
    flags |= sampler ? flags::SAMPLER_HEAP : flags::RESOURCE_HEAP;
    nonuniform_handles += nonuniform ? 1u : 0u;
    const V s = m.bool_(sampler);
    const V n = m.bool_(nonuniform);
    const V h = op(block, "createHandleFromHeap", Attr::ReadNone, t.handle, 218, {clamped, s, n});
    return annotate(block, h, props.first, props.second);
}

std::vector<std::uint8_t> Shader::finish()
{
    const std::uint32_t ud_size = next_multiple_of(user_data * 4, 16);
    fix_user_data_size(ud_size);
    const std::uint32_t passes = shader_opt() & opt::MODULE;
    if (passes != 0) {
        m.optimise(passes);
        const Usage used = m.usage();
        const std::array<std::pair<std::uint64_t, bool>, 6> checks{{
            {flags::WAVE_OPS, used.wave_ops},
            {flags::INT64, used.int64},
            {flags::TYPED_UAV_LOADS, used.typed_uav_loads},
            {flags::RESOURCE_HEAP, used.heap_views},
            {flags::SAMPLER_HEAP, used.heap_samplers},
            {flags::ATOMIC_TYPED64 | flags::ATOMIC_HEAP64, used.atomic64},
        }};
        for (const auto& [flag, is_used] : checks) {
            if (!is_used) {
                flags &= ~flag;
            }
        }
    }
    std::optional<Md> resources;
    std::vector<Binding> bindings;
    const auto i32c = [this](std::uint32_t v) {
        const V c = m.i32(v);
        return m.md_value(c);
    };
    if (user_data > 0 || samplers > 0 || !root_cbvs.empty() || !root_srvs.empty() || !root_uavs.empty()) {
        const Md empty = m.md_str("");
        std::optional<Md> cbvs;
        std::vector<std::optional<Md>> cbv_list;
        if (user_data > 0) {
            const Ty arr = m.array(next_multiple_of(user_data, 4), t.i32);
            const Ty ty = m.named_struct("UserData", {arr});
            const Ty p = m.ptr(ty);
            const V u = m.undef(p);
            std::vector<std::optional<Md>> ops;
            ops.push_back(i32c(0));
            ops.push_back(m.md_value(u));
            ops.push_back(empty);
            ops.push_back(i32c(USER_DATA_SPACE));
            ops.push_back(i32c(0));
            ops.push_back(i32c(1));
            ops.push_back(i32c(ud_size));
            ops.push_back(std::nullopt);
            cbv_list.push_back(m.md_node(std::move(ops)));
            bindings.push_back(Binding{2, USER_DATA_SPACE, 0, 13});
        }
        const Ty blob = m.array(16384, t.i32);
        const Ty root_ty = m.named_struct("RootCbv", {blob});
        const Ty root_p = m.ptr(root_ty);
        const V root_u = m.undef(root_p);
        const auto cbv_slots = root_cbvs;
        for (const auto& [slot, h] : cbv_slots) {
            std::vector<std::optional<Md>> ops;
            ops.push_back(i32c(static_cast<std::uint32_t>(cbv_list.size())));
            ops.push_back(m.md_value(root_u));
            ops.push_back(empty);
            ops.push_back(i32c(ROOT_CBV_SPACE));
            ops.push_back(i32c(slot));
            ops.push_back(i32c(1));
            ops.push_back(i32c(65536));
            ops.push_back(std::nullopt);
            cbv_list.push_back(m.md_node(std::move(ops)));
            bindings.push_back(Binding{2, ROOT_CBV_SPACE, slot, 13});
        }
        if (!cbv_list.empty()) {
            cbvs = m.md_node(std::move(cbv_list));
        }
        std::optional<Md> srvs;
        if (!root_srvs.empty()) {
            const Ty ty = m.named_struct("struct.ByteAddressBuffer", {t.i32});
            const Ty p = m.ptr(ty);
            const V u = m.undef(p);
            std::vector<std::optional<Md>> list;
            const auto slots = root_srvs;
            for (std::size_t n = 0; n < slots.size(); ++n) {
                const std::uint8_t slot = slots[n].first;
                std::vector<std::optional<Md>> ops;
                ops.push_back(i32c(static_cast<std::uint32_t>(n)));
                ops.push_back(m.md_value(u));
                ops.push_back(empty);
                ops.push_back(i32c(ROOT_CBV_SPACE));
                ops.push_back(i32c(slot));
                ops.push_back(i32c(1));
                ops.push_back(i32c(11));
                ops.push_back(i32c(0));
                ops.push_back(std::nullopt);
                list.push_back(m.md_node(std::move(ops)));
                bindings.push_back(Binding{4, ROOT_CBV_SPACE, slot, 11});
            }
            srvs = m.md_node(std::move(list));
        }
        std::optional<Md> uavs;
        if (!root_uavs.empty()) {
            const Ty ty = m.named_struct("struct.RWByteAddressBuffer", {t.i32});
            const Ty p = m.ptr(ty);
            const V u = m.undef(p);
            const V no_v = m.bool_(false);
            const Md no = m.md_value(no_v);
            std::vector<std::optional<Md>> list;
            const auto slots = root_uavs;
            for (std::size_t n = 0; n < slots.size(); ++n) {
                const std::uint8_t slot = slots[n].first;
                std::vector<std::optional<Md>> ops;
                ops.push_back(i32c(static_cast<std::uint32_t>(n)));
                ops.push_back(m.md_value(u));
                ops.push_back(empty);
                ops.push_back(i32c(ROOT_CBV_SPACE));
                ops.push_back(i32c(slot));
                ops.push_back(i32c(1));
                ops.push_back(i32c(11));
                ops.push_back(no);
                ops.push_back(no);
                ops.push_back(no);
                ops.push_back(std::nullopt);
                list.push_back(m.md_node(std::move(ops)));
                bindings.push_back(Binding{7, ROOT_CBV_SPACE, slot, 11});
            }
            uavs = m.md_node(std::move(list));
        }
        std::optional<Md> sampler_list;
        if (samplers > 0) {
            const Ty ty = m.named_struct("struct.SamplerState", {t.i32});
            const Ty p = m.ptr(ty);
            const V u = m.undef(p);
            std::vector<std::optional<Md>> list;
            for (std::uint32_t n = 0; n < samplers; ++n) {
                const auto kind = static_cast<std::uint32_t>((cmp_samplers >> n) & 1);
                std::vector<std::optional<Md>> ops;
                ops.push_back(i32c(n));
                ops.push_back(m.md_value(u));
                ops.push_back(empty);
                ops.push_back(i32c(SAMPLER_SPACE));
                ops.push_back(i32c(n));
                ops.push_back(i32c(1));
                ops.push_back(i32c(kind));
                ops.push_back(std::nullopt);
                list.push_back(m.md_node(std::move(ops)));
                bindings.push_back(Binding{1, SAMPLER_SPACE, n, 14});
            }
            sampler_list = m.md_node(std::move(list));
        }
        resources = m.md_node({srvs, uavs, cbvs, sampler_list});
        std::stable_sort(bindings.begin(), bindings.end(), [](const Binding& a, const Binding& b) {
            return binding_rank(a.kind) < binding_rank(b.kind);
        });
    }
    if (resources) {
        m.named_md("dx.resources", {*resources});
    }

    const bool graphics = !threads.has_value() || mesh.has_value();
    if (graphics) {
        const std::uint32_t ni = scalars(inputs);
        const std::uint32_t no = scalars(outputs);
        std::vector<std::uint32_t> view{ni, no};
        if (mesh) {
            view.push_back(scalars(prims));
        }
        for (std::uint32_t i = 0; i < ni; ++i) {
            for (std::uint32_t k = 0; k < (no + 31) / 32; ++k) {
                const std::uint32_t bits = std::min(no - 32 * k, 32u);
                view.push_back(bits == 32 ? kMax32 : (1u << bits) - 1);
            }
        }
        if (geometry) {
            view.insert(view.end(), {0, 0, 0});
        }
        const Ty arr_ty = m.array(static_cast<std::uint32_t>(view.size()), t.i32);
        std::vector<V> elems;
        elems.reserve(view.size());
        for (const std::uint32_t v : view) {
            elems.push_back(m.i32(v));
        }
        const V arr = m.aggregate(arr_ty, elems);
        const Md arr_md = m.md_value(arr);
        const Md view_node = m.md_node({arr_md});
        m.named_md("dx.viewIdState", {view_node});
    }

    std::optional<Md> sigs;
    if (graphics) {
        const auto ins = signature_md(inputs);
        const auto outs = signature_md(outputs);
        const auto prim_sig = signature_md(prims);
        sigs = m.md_node({ins, outs, prim_sig});
    }
    if (pixel && std::any_of(outputs.begin(), outputs.end(), [](const SigElement& o) { return o.kind == 20; })) {
        flags |= flags::STENCIL_REF;
    }
    std::vector<Md> props;
    if (flags != 0) {
        props.push_back(i32c(0));
        const Ty i64t = m.int_(64);
        const V f = m.int_const(i64t, static_cast<std::int64_t>(flags));
        props.push_back(m.md_value(f));
    }
    if (geometry) {
        props.push_back(i32c(1));
        std::vector<std::optional<Md>> state;
        for (const std::uint32_t n : {3u, geometry->max_vertices, 1u, 5u, 1u}) {
            state.push_back(i32c(n));
        }
        props.push_back(m.md_node(std::move(state)));
    }
    if (threads) {
        std::vector<std::optional<Md>> tops;
        for (const std::uint32_t n : *threads) {
            tops.push_back(i32c(n));
        }
        const Md tnode = m.md_node(std::move(tops));
        if (mesh) {
            props.push_back(i32c(9));
            std::vector<std::optional<Md>> node{tnode};
            for (const std::uint32_t n : {mesh->max_vertices, mesh->max_primitives, static_cast<std::uint32_t>(mesh->topology), 0u}) {
                node.push_back(i32c(n));
            }
            props.push_back(m.md_node(std::move(node)));
        } else {
            props.push_back(i32c(4));
            props.push_back(tnode);
        }
    }
    if (wave_size) {
        props.push_back(i32c(11));
        const Md n = i32c(*wave_size);
        props.push_back(m.md_node({n}));
    }
    std::optional<Md> props_node;
    if (!props.empty()) {
        std::vector<std::optional<Md>> ops(props.begin(), props.end());
        props_node = m.md_node(std::move(ops));
    }
    const Md main_md = m.md_value(main_);
    const Md name_md = m.md_str("main");
    const Md entry_md = m.md_node({main_md, name_md, sigs, resources, props_node});

    const Md ident_str = m.md_str("euphoria");
    const Md ident = m.md_node({ident_str});
    m.named_md("llvm.ident", {ident});
    const Md one = i32c(1);
    const Md six = i32c(6);
    const Md version = m.md_node({one, six});
    m.named_md("dx.version", {version});
    m.named_md("dx.valver", {version});
    std::string_view model = "vs";
    std::uint32_t kind = 1;
    if (mesh) {
        model = "ms";
        kind = 13;
    } else if (geometry) {
        model = "gs";
        kind = 2;
    } else if (threads) {
        model = "cs";
        kind = 5;
    } else if (pixel) {
        model = "ps";
        kind = 0;
    }
    const Md model_md = m.md_str(model);
    const Md sm = m.md_node({model_md, six, six});
    m.named_md("dx.shaderModel", {sm});
    m.named_md("dx.entryPoints", {entry_md});

    const std::vector<std::uint8_t> bitcode = m.write();
    census::note(m, bitcode);
    std::vector<std::uint8_t> dxil;
    dxil.reserve(24 + bitcode.size());
    put32(dxil, (kind << 16) | 0x66);
    put32(dxil, static_cast<std::uint32_t>((24 + bitcode.size()) / 4));
    dxil.insert(dxil.end(), {'D', 'X', 'I', 'L'});
    put32(dxil, 0x106);
    put32(dxil, 16);
    put32(dxil, static_cast<std::uint32_t>(bitcode.size()));
    dxil.insert(dxil.end(), bitcode.begin(), bitcode.end());

    std::uint64_t features = 0;
    if ((flags & flags::DOUBLES) != 0) {
        features |= 0x1;
    }
    if ((flags & flags::STENCIL_REF) != 0) {
        features |= 0x200;
    }
    if ((flags & flags::TYPED_UAV_LOADS) != 0) {
        features |= 0x800;
    }
    if ((flags & flags::WAVE_OPS) != 0) {
        features |= 0x4000;
    }
    if ((flags & flags::INT64) != 0) {
        features |= 0x8000;
    }
    if ((flags & flags::BARYCENTRICS) != 0) {
        features |= 0x20000;
    }
    if ((flags & flags::RESOURCE_HEAP) != 0) {
        features |= 0x2000000;
    }
    if ((flags & flags::SAMPLER_HEAP) != 0) {
        features |= 0x4000000;
    }
    if ((flags & flags::ATOMIC_TYPED64) != 0) {
        features |= 0x400000;
    }
    if ((flags & flags::ATOMIC_HEAP64) != 0) {
        features |= 0x10000000;
    }
    std::vector<std::uint8_t> sfi0;
    put32(sfi0, static_cast<std::uint32_t>(features));
    put32(sfi0, static_cast<std::uint32_t>(features >> 32));
    const std::vector<std::uint8_t> isg1 = signature(inputs, true);
    const std::vector<std::uint8_t> osg1 = signature(outputs, false);
    bool flag = false;
    if (pixel) {
        flag = std::any_of(outputs.begin(), outputs.end(), [](const SigElement& o) { return o.kind >= 17 && o.kind <= 19; });
    } else {
        flag = std::any_of(outputs.begin(), outputs.end(), [](const SigElement& o) { return o.kind == 3; });
    }
    Psv p;
    p.stage = static_cast<std::uint8_t>(kind);
    p.flag = flag && !threads.has_value();
    p.inputs = inputs;
    p.outputs = outputs;
    p.prims = prims;
    if (mesh) {
        p.mesh = std::pair{*mesh, lds_dwords * 4};
    }
    if (geometry) {
        p.geometry = geometry->max_vertices;
    }
    p.bindings = bindings;
    p.threads = threads.value_or(std::array<std::uint32_t, 3>{});
    p.wave_size = wave_size;
    const std::vector<std::uint8_t> psv0 = psv(p);
    const std::array<std::uint8_t, 20> hash_part{};
    const std::vector<std::uint8_t> psg1 = signature(prims, false);
    std::vector<ContainerPart> parts{
        part("SFI0", sfi0),
        part("ISG1", isg1),
        part("OSG1", osg1),
    };
    if (mesh) {
        parts.push_back(part("PSG1", psg1));
    }
    parts.push_back(part("PSV0", psv0));
    parts.push_back(part("HASH", hash_part));
    parts.push_back(part("DXIL", dxil));
    return build(parts);
}

void Shader::fix_user_data_size(std::uint32_t size)
{
    const V o0 = m.i32(13);
    const V o1 = m.i32(kMax32);
    const V old = m.aggregate(t.props, {o0, o1});
    const V n0 = m.i32(13);
    const V n1 = m.i32(size);
    const V fresh = m.aggregate(t.props, {n0, n1});
    m.replace_const(old, fresh);
}

std::optional<Md> Shader::signature_md(const std::vector<SigElement>& elements)
{
    if (elements.empty()) {
        return std::nullopt;
    }
    std::vector<std::optional<Md>> list;
    for (std::size_t id = 0; id < elements.size(); ++id) {
        const SigElement& e = elements[id];
        const auto c32 = [this](std::uint32_t v) -> std::optional<Md> {
            const V c = m.i32(v);
            return m.md_value(c);
        };
        const auto c8 = [this](std::uint8_t v) -> std::optional<Md> {
            const V c = m.i8(v);
            return m.md_value(c);
        };
        const auto idx = c32(e.index);
        const std::optional<Md> indices = m.md_node({idx});
        std::optional<Md> usage;
        if (e.usage != 0) {
            const auto tag = c32(3);
            const auto mask = c32(e.usage);
            usage = m.md_node({tag, mask});
        }
        std::vector<std::optional<Md>> ops;
        ops.push_back(c32(static_cast<std::uint32_t>(id)));
        ops.push_back(m.md_str(e.name));
        ops.push_back(c8(e.comp));
        ops.push_back(c8(e.kind));
        ops.push_back(indices);
        ops.push_back(c8(e.interp));
        ops.push_back(c32(1));
        ops.push_back(c8(e.cols));
        ops.push_back(c32(e.row.value_or(kMax32)));
        ops.push_back(c8(e.row ? e.col : std::uint8_t{0xFF}));
        ops.push_back(usage);
        list.push_back(m.md_node(std::move(ops)));
    }
    return m.md_node(std::move(list));
}

}
