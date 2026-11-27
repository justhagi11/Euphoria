#include <euphoria/shader/gfx10/structure.hpp>

#include <algorithm>
#include <string_view>
#include <utility>

namespace euphoria::shader::gfx10::structure {
namespace {

constexpr std::uint32_t kTerminal = 0xFFFFFFFFu;
constexpr std::size_t kMaxNodes = std::size_t{1} << 21;
constexpr std::size_t kMaxMeet = std::size_t{1} << 16;
constexpr std::size_t kMaxMeets = std::size_t{1} << 18;
constexpr std::size_t kMaxRounds = 256;

constexpr ShaderError kTooLarge = ShaderError::Isa(0, "lane conditions too large");

std::uint64_t pack3(std::uint32_t a, std::uint32_t b, std::uint32_t c) noexcept
{
    return (static_cast<std::uint64_t>(a) << 42) | (static_cast<std::uint64_t>(b) << 21) | c;
}

std::optional<std::string_view> strip_prefix(std::string_view text, std::string_view prefix)
{
    if (!text.starts_with(prefix)) {
        return std::nullopt;
    }
    return text.substr(prefix.size());
}

std::optional<std::string_view> strip_suffix(std::string_view text, std::string_view suffix)
{
    if (!text.ends_with(suffix)) {
        return std::nullopt;
    }
    return text.substr(0, text.size() - suffix.size());
}

bool ends_with(std::string_view text, std::string_view suffix)
{
    return text.ends_with(suffix);
}

}

Bools::Bools()
{
    nodes_.push_back(Node{kTerminal, Bool::False, Bool::False});
    nodes_.push_back(Node{kTerminal, Bool::True, Bool::True});
}

std::optional<std::tuple<Atom, Bool, Bool>> Bools::node(Bool b) const
{
    if (b.id >= nodes_.size()) {
        return std::nullopt;
    }
    const Node& node = nodes_[b.id];
    if (node.var == kTerminal) {
        return std::nullopt;
    }
    return std::tuple{atoms_[node.var], node.low, node.high};
}

std::vector<std::uint32_t> Bools::vars(Bool b) const
{
    std::vector<Bool> seen;
    std::vector<std::uint32_t> found;
    std::vector<Bool> stack{b};
    while (!stack.empty()) {
        const Bool current = stack.back();
        stack.pop_back();
        if (current.id >= nodes_.size()) {
            continue;
        }
        const Node& node = nodes_[current.id];
        if (node.var == kTerminal
            || std::find(seen.begin(), seen.end(), current) != seen.end()) {
            continue;
        }
        seen.push_back(current);
        if (std::find(found.begin(), found.end(), node.var) == found.end()) {
            found.push_back(node.var);
        }
        stack.push_back(node.low);
        stack.push_back(node.high);
    }
    return found;
}

std::vector<Atom> Bools::support(Bool b) const
{
    std::vector<std::uint32_t> found = vars(b);
    std::sort(found.begin(), found.end());
    std::vector<Atom> atoms;
    atoms.reserve(found.size());
    for (const std::uint32_t var : found) {
        atoms.push_back(atoms_[var]);
    }
    return atoms;
}

Bool Bools::atom(Atom atom, std::size_t born)
{
    std::uint32_t var = 0;
    const auto entry = ids_.find(atom);
    if (entry != ids_.end()) {
        var = entry->second;
    } else {
        atoms_.push_back(atom);
        born_.push_back(born);
        var = static_cast<std::uint32_t>(atoms_.size() - 1);
        ids_.emplace(atom, var);
    }
    return mk(var, Bool::False, Bool::True);
}

Bool Bools::mk(std::uint32_t v, Bool lo, Bool hi)
{
    if (lo == hi) {
        return lo;
    }
    const std::uint64_t key = pack3(v, lo.id, hi.id);
    if (const auto found = unique_.find(key); found != unique_.end()) {
        return found->second;
    }
    if (nodes_.size() >= kMaxNodes) {
        overflow_ = true;
        return Bool::False;
    }
    const Bool made{static_cast<std::uint32_t>(nodes_.size())};
    nodes_.push_back(Node{v, lo, hi});
    unique_.emplace(key, made);
    return made;
}

std::uint32_t Bools::top(Bool b) const
{
    return nodes_[b.id].var;
}

std::pair<Bool, Bool> Bools::split(Bool b, std::uint32_t v) const
{
    const Node& node = nodes_[b.id];
    if (node.var == v) {
        return {node.low, node.high};
    }
    return {b, b};
}

Bool Bools::ite(Bool f, Bool g, Bool h)
{
    if (f == Bool::True || g == h) {
        return g;
    }
    if (f == Bool::False) {
        return h;
    }
    if (g == Bool::True && h == Bool::False) {
        return f;
    }
    const std::uint64_t key = pack3(f.id, g.id, h.id);
    if (const auto found = memo_.find(key); found != memo_.end()) {
        return found->second;
    }
    const std::uint32_t v = std::min({top(f), top(g), top(h)});
    const auto [f0, f1] = split(f, v);
    const auto [g0, g1] = split(g, v);
    const auto [h0, h1] = split(h, v);
    const Bool lo = ite(f0, g0, h0);
    const Bool hi = ite(f1, g1, h1);
    const Bool made = mk(v, lo, hi);
    memo_.emplace(key, made);
    return made;
}

Bool Bools::and_(Bool a, Bool b)
{
    return ite(a, b, Bool::False);
}

Bool Bools::or_(Bool a, Bool b)
{
    return ite(a, Bool::True, b);
}

Bool Bools::not_(Bool a)
{
    return ite(a, Bool::False, Bool::True);
}

Bool Bools::xor_(Bool a, Bool b)
{
    return ite(a, not_(b), b);
}

Bool Bools::land_not(Bool a, Bool b)
{
    return and_(a, not_(b));
}

Bool Bools::lor_not(Bool a, Bool b)
{
    return or_(a, not_(b));
}

bool Bools::implies(Bool a, Bool b)
{
    return and_(a, not_(b)) == Bool::False;
}

bool Bools::meet(std::span<const std::pair<Bool, bool>> all) const
{
    Meets seen;
    return meet_seen(all, seen);
}

bool Bools::meet_seen(std::span<const std::pair<Bool, bool>> all, Meets& seen) const
{
    return meet_within(all, kMaxMeet, seen);
}

bool Bools::meet_within(std::span<const std::pair<Bool, bool>> all, std::size_t budget,
                        Meets& seen) const
{
    std::vector<std::uint32_t> work;
    work.reserve(all.size() + 64);
    for (const auto& [b, holds] : all) {
        work.push_back((b.id << 1) | (holds ? 1u : 0u));
    }
    return meet_in(0, work, budget, seen);
}

bool Bools::meet_in(std::size_t at, std::vector<std::uint32_t>& work, std::size_t& budget,
                    Meets& seen) const
{
    std::size_t kept = at;
    for (std::size_t i = at; i < work.size(); ++i) {
        const std::uint32_t b = work[i] >> 1;
        const bool holds = (work[i] & 1u) != 0;
        if (b > Bool::True.id) {
            work[kept] = work[i];
            ++kept;
        } else if ((b == Bool::True.id) != holds) {
            work.resize(at);
            return false;
        }
    }
    work.resize(kept);
    if (kept == at || budget == 0) {
        work.resize(at);
        return true;
    }
    std::sort(work.begin() + static_cast<std::ptrdiff_t>(at),
              work.begin() + static_cast<std::ptrdiff_t>(kept));
    std::size_t end = at + 1;
    for (std::size_t i = at + 1; i < kept; ++i) {
        if (work[i] != work[end - 1]) {
            work[end] = work[i];
            ++end;
        }
    }
    work.resize(end);
    const std::vector<std::uint32_t> condition(work.begin() + static_cast<std::ptrdiff_t>(at),
                                               work.begin() + static_cast<std::ptrdiff_t>(end));
    if (const auto found = seen.none.find(condition); found != seen.none.end()) {
        const std::size_t splits = found->second;
        const bool within = splits <= budget;
        budget = splits > budget ? 0 : budget - splits;
        work.resize(at);
        return !within;
    }
    const std::size_t before = budget;
    --budget;
    std::uint32_t v = 0xFFFFFFFFu;
    for (std::size_t i = at; i < end; ++i) {
        v = std::min(v, nodes_[work[i] >> 1].var);
    }
    bool found = false;
    for (const bool value : {false, true}) {
        for (std::size_t i = at; i < end; ++i) {
            const auto [lo, hi] = split(Bool{work[i] >> 1}, v);
            work.push_back(((value ? hi : lo).id << 1) | (work[i] & 1u));
        }
        if (meet_in(end, work, budget, seen)) {
            found = true;
            break;
        }
    }
    if (!found && seen.none.size() < kMaxMeets) {
        const std::vector<std::uint32_t> failed(work.begin() + static_cast<std::ptrdiff_t>(at),
                                                work.begin() + static_cast<std::ptrdiff_t>(end));
        seen.none.emplace(failed, before - budget);
    }
    work.resize(at);
    return found;
}

Bool Bools::forget(Bool b, std::size_t from, std::unordered_map<std::uint32_t, Bool>& memo)
{
    const Node& node = nodes_[b.id];
    if (node.var == kTerminal) {
        return b;
    }
    if (const auto found = memo.find(b.id); found != memo.end()) {
        return found->second;
    }
    const Bool lo = forget(node.low, from, memo);
    const Bool hi = forget(node.high, from, memo);
    const Bool made = born_[node.var] >= from ? or_(lo, hi) : mk(node.var, lo, hi);
    memo.emplace(b.id, made);
    return made;
}

Bool Bools::restrict(Bool b, std::uint32_t v, bool value,
                     std::unordered_map<std::uint32_t, Bool>& memo)
{
    const Node& node = nodes_[b.id];
    if (node.var == kTerminal || node.var > v) {
        return b;
    }
    if (node.var == v) {
        return value ? node.high : node.low;
    }
    if (const auto found = memo.find(b.id); found != memo.end()) {
        return found->second;
    }
    const Bool lo = restrict(node.low, v, value, memo);
    const Bool hi = restrict(node.high, v, value, memo);
    const Bool made = mk(node.var, lo, hi);
    memo.emplace(b.id, made);
    return made;
}

Bool Bools::given(Bool b, Bool known)
{
    for (const std::uint32_t v : vars(b)) {
        const Bool x = mk(v, Bool::False, Bool::True);
        const Bool nx = not_(x);
        if (implies(known, x)) {
            std::unordered_map<std::uint32_t, Bool> memo;
            b = restrict(b, v, true, memo);
        } else if (implies(known, nx)) {
            std::unordered_map<std::uint32_t, Bool> memo;
            b = restrict(b, v, false, memo);
        }
    }
    return b;
}

std::optional<Bool> Structured::mask_bits(ir::Value low, std::optional<ir::Value> high) const
{
    if (high) {
        if (const auto found = paired_masks.find(MaskKey{low, *high}); found != paired_masks.end()) {
            return found->second;
        }
        return std::nullopt;
    }
    if (const auto found = masks.find(low); found != masks.end()) {
        return found->second;
    }
    return std::nullopt;
}

namespace {

struct Join {
    std::size_t block{};
    ir::Value id{};
    bool value{false};
    bool derived{false};
    std::vector<MaskKey> args;
};

struct PhiJoin {
    std::size_t block{};
    ir::Value id{};
    bool derived{false};
    std::vector<MaskKey> args;
};

struct SopMasks {
    std::optional<std::pair<MaskKey, Bool>> dst;
    std::optional<std::pair<MaskKey, Bool>> exec;
    bool derived{false};
    std::optional<Bool> scc;
};

class Lanes {
public:
    static std::expected<Lanes, ShaderError> create(const ir::Program& program)
    {
        Lanes lanes{program};
        const std::vector<std::size_t> order = ir::reverse_postorder(program.blocks);
        lanes.order_ = order;
        lanes.index_.assign(program.blocks.size(), kNoIndex);
        for (std::size_t i = 0; i < order.size(); ++i) {
            lanes.index_[order[i]] = i;
        }
        if (const auto bad = ir::irreducible_edge(program.blocks, lanes.order_, lanes.index_)) {
            return std::unexpected(ShaderError::Unsupported(
                program.blocks[bad->first].offset, "irreducible control flow"));
        }
        lanes.incoming_.resize(program.blocks.size());
        for (std::size_t b = 0; b < program.blocks.size(); ++b) {
            const auto successors = program.blocks[b].exit.successors();
            for (std::size_t k = 0; k < successors.size(); ++k) {
                lanes.incoming_[successors[k]].push_back({b, k == 0});
            }
            for (std::size_t i = 0; i < program.blocks[b].phis.size(); ++i) {
                lanes.phis_.emplace(program.blocks[b].phis[i].value, std::pair{b, i});
            }
            for (std::size_t i = 0; i < program.blocks[b].insts.size(); ++i) {
                for (const auto& write : program.blocks[b].insts[i].writes) {
                    lanes.defs_.emplace(write.second, std::pair{b, i});
                }
            }
        }
        lanes.first_.assign(program.blocks.size(), Bool::False);
        lanes.when_.assign(program.blocks.size(), std::nullopt);
        lanes.wave_any_.assign(program.blocks.size(), std::nullopt);
        return lanes;
    }

    void set_wide(const std::unordered_set<std::size_t>& wide) { wide_ = wide; }

    bool overflowed() const { return b_.overflowed(); }

    void adopt_unresolved(const std::vector<ir::Value>& wrong,
                         const std::vector<ir::Value>& new_derived)
    {
        unresolved_.insert(wrong.begin(), wrong.end());
        derived_phis_.insert(new_derived.begin(), new_derived.end());
    }

    void reset()
    {
        b_ = Bools{};
        masks_.clear();
        other_.clear();
        seen_.clear();
        scc_lane_.clear();
        scc_every_.clear();
        wave_any_.assign(p_->blocks.size(), std::nullopt);
        joins_.clear();
        phi_joins_.clear();
        phi_join_ids_.clear();
        first_.assign(p_->blocks.size(), Bool::False);
        when_.assign(p_->blocks.size(), std::nullopt);
        active_.clear();
    }

    std::vector<std::vector<std::optional<Bool>>> masks_in_order()
    {
        const auto input = [this](ir::Reg reg) -> std::optional<ir::Value> {
            for (const auto& entry : p_->inputs) {
                if (entry.first == reg) {
                    return entry.second;
                }
            }
            return std::nullopt;
        };
        if (const auto lo = input(ir::Reg::S(ir::EXEC))) {
            const ir::Value hi = p_->wave64
                                     ? input(ir::Reg::S(static_cast<std::uint8_t>(ir::EXEC + 1))).value_or(*lo)
                                     : *lo;
            masks_.emplace(MaskKey{*lo, hi}, Bool::True);
            active_.insert(*lo);
        }
        std::vector<std::vector<std::optional<Bool>>> execs(p_->blocks.size());
        for (std::size_t block = 0; block < p_->blocks.size(); ++block) {
            execs[block].assign(p_->blocks[block].insts.size(), std::nullopt);
        }
        for (std::size_t n = 0; n < order_.size(); ++n) {
            const std::size_t b = order_[n];
            Bool c = n == 0 ? Bool::True : Bool::False;
            for (const auto& [q, taken] : incoming_[b]) {
                if (index_[q] < n) {
                    const Bool e = edge(first_[q], q, taken);
                    c = b_.or_(c, e);
                }
            }
            first_[b] = c;
            for (const ir::Phi& phi : p_->blocks[b].phis) {
                if (phi.reg == ir::Reg::S(ir::EXEC)) {
                    active_.insert(phi.value);
                }
            }
            std::optional<Bool> now;
            for (std::size_t i = 0; i < p_->blocks[b].insts.size(); ++i) {
                const ir::Inst& inst = p_->blocks[b].insts[i];
                if (inst.read(ir::Reg::S(ir::EXEC))) {
                    const Bool e = exec(inst);
                    execs[b][i] = e;
                    now = e;
                }
                step(inst, now);
                if (const auto k = key_write(inst, ir::EXEC)) {
                    const auto found = masks_.find(*k);
                    if (found != masks_.end()) {
                        now = found->second;
                    } else if (const auto other = other_.find(*k); other != other_.end()) {
                        now = other->second;
                    } else {
                        now = std::nullopt;
                    }
                }
            }
            if (p_->blocks[b].exit.kind == ir::Exit::Kind::Branch
                && !p_->blocks[b].insts.empty()) {
                const ir::Inst& last = p_->blocks[b].insts.back();
                when_[b] = branch(last);
                wave_any_[b] = wave_kind(last);
            }
        }
        return execs;
    }

    Bool edge(Bool c, std::size_t q, bool taken)
    {
        if (wide_.contains(q)) {
            return c;
        }
        if (!when_[q]) {
            return c;
        }
        const Bool w = *when_[q];
        if (taken) {
            return b_.and_(c, w);
        }
        return b_.land_not(c, w);
    }

    std::pair<std::vector<ir::Value>, std::vector<ir::Value>> check_joins(
        const std::vector<Bool>& cond)
    {
        std::vector<ir::Value> wrong;
        std::vector<ir::Value> new_derived;
        std::unordered_set<ir::Value> seen_derived;
        std::size_t j = 0;
        std::size_t pindex = 0;
        while (j < joins_.size() || pindex < phi_joins_.size()) {
            while (j < joins_.size()) {
                const Join join = joins_[j];
                bool failed = false;
                for (std::size_t n = 0; n < join.args.size(); ++n) {
                    const auto& [q, taken] = incoming_[join.block][n];
                    const Bool e = edge(cond[q], q, taken);
                    auto [m, d] = mask(join.args[n]);
                    if (!join.derived && d && !derived_phis_.contains(join.id)
                        && seen_derived.insert(join.id).second) {
                        new_derived.push_back(join.id);
                    }
                    if (!join.value) {
                        m = b_.not_(m);
                    }
                    if (!failed && !b_.implies(e, m)) {
                        wrong.push_back(join.id);
                        failed = true;
                    }
                }
                ++j;
            }
            while (pindex < phi_joins_.size()) {
                const PhiJoin join = phi_joins_[pindex];
                for (const MaskKey& a : join.args) {
                    const auto [m, d] = mask(a);
                    if (!join.derived && d && !derived_phis_.contains(join.id)
                        && seen_derived.insert(join.id).second) {
                        new_derived.push_back(join.id);
                    }
                }
                ++pindex;
            }
        }
        return {std::move(wrong), std::move(new_derived)};
    }

    std::expected<Structured, ShaderError> finish(
        const std::vector<std::vector<std::optional<Bool>>>& execs,
        const std::vector<Bool>& cond)
    {
        std::vector<PhiJoin> phi_joins = std::move(phi_joins_);
        std::unordered_map<ir::Value, std::vector<Bool>> phi_masks;
        phi_masks.reserve(phi_joins.size());
        for (const PhiJoin& join : phi_joins) {
            std::vector<Bool> incoming;
            incoming.reserve(join.args.size());
            for (std::size_t n = 0; n < join.args.size(); ++n) {
                const auto& [q, taken] = incoming_[join.block][n];
                const Bool e = edge(cond[q], q, taken);
                const Bool m = mask(join.args[n]).first;
                incoming.push_back(b_.given(m, e));
            }
            phi_masks.emplace(join.id, std::move(incoming));
        }
        std::vector<std::vector<std::optional<Bool>>> guards;
        guards.reserve(p_->blocks.size());
        for (std::size_t n = 0; n < p_->blocks.size(); ++n) {
            std::vector<std::optional<Bool>> block_guards;
            block_guards.reserve(execs[n].size());
            for (std::size_t i = 0; i < execs[n].size(); ++i) {
                const std::optional<Bool>& exec = execs[n][i];
                const ir::Fields& fields = p_->blocks[n].insts[i].fields;
                if (!exec || fields.is<ir::Fields::Sop>() || fields.is<ir::Fields::Sopp>()
                    || fields.is<ir::Fields::Smem>()) {
                    block_guards.push_back(std::nullopt);
                    continue;
                }
                if (b_.implies(cond[n], *exec)) {
                    block_guards.push_back(std::nullopt);
                    continue;
                }
                block_guards.push_back(b_.given(*exec, cond[n]));
            }
            guards.push_back(std::move(block_guards));
        }
        std::vector<LaneExit> exits;
        exits.reserve(p_->blocks.size());
        for (std::size_t n = 0; n < p_->blocks.size(); ++n) {
            const ir::Exit& exit = p_->blocks[n].exit;
            LaneExit lane_exit{};
            switch (exit.kind) {
            case ir::Exit::Kind::Jump:
                lane_exit.kind = LaneExit::Kind::Jump;
                lane_exit.target = exit.target;
                break;
            case ir::Exit::Kind::End:
                lane_exit.kind = LaneExit::Kind::End;
                break;
            case ir::Exit::Kind::Indirect:
                lane_exit.kind = LaneExit::Kind::Indirect;
                break;
            case ir::Exit::Kind::Branch:
                lane_exit.kind = LaneExit::Kind::Branch;
                lane_exit.when = b_.given(when_[n].value_or(Bool::False), cond[n]);
                lane_exit.taken = exit.taken;
                lane_exit.not_taken = exit.not_taken;
                break;
            }
            exits.push_back(lane_exit);
        }
        if (b_.overflowed()) {
            return std::unexpected(kTooLarge);
        }
        std::unordered_map<ir::Value, ir::Value> high;
        std::unordered_set<ir::Value> split_low;
        std::vector<MaskKey> keys;
        keys.reserve(masks_.size() + other_.size() + seen_.size());
        for (const auto& entry : masks_) {
            keys.push_back(entry.first);
        }
        for (const auto& entry : other_) {
            keys.push_back(entry.first);
        }
        for (const MaskKey& key : seen_) {
            keys.push_back(key);
        }
        for (const MaskKey& key : keys) {
            const auto [found, inserted] = high.try_emplace(key.low, key.high);
            if (!inserted && found->second != key.high) {
                split_low.insert(key.low);
            }
        }
        for (auto entry = masks_.begin(); entry != masks_.end();) {
            entry = split_low.contains(entry->first.low) ? masks_.erase(entry) : std::next(entry);
        }
        for (auto entry = active_.begin(); entry != active_.end();) {
            entry = split_low.contains(*entry) ? active_.erase(entry) : std::next(entry);
        }
        Structured result{};
        for (const auto& entry : masks_) {
            result.masks.emplace(entry.first.low, entry.second);
        }
        for (const auto& entry : masks_) {
            if (entry.first.low != entry.first.high) {
                result.highs.emplace(entry.first.high, entry.first.low);
            }
        }
        if (p_->wave64) {
            result.paired_masks = masks_;
        }
        result.active = std::move(active_);
        result.wide = wide_;
        result.wave_any = wave_any_;
        for (const auto& entry : scc_lane_) {
            if (!scc_every_.contains(entry.first)) {
                result.scc_some.emplace(entry.first, entry.second);
            }
        }
        result.phi_masks = std::move(phi_masks);
        result.bools = std::move(b_);
        result.exits = std::move(exits);
        result.guards = std::move(guards);
        result.reach = cond;
        return result;
    }

    static constexpr std::size_t kNoIndex = static_cast<std::size_t>(-1);

    explicit Lanes(const ir::Program& program) : p_(&program) {}

    std::optional<MaskKey> key_read(const ir::Inst& inst, std::uint8_t code) const
    {
        const auto lo = inst.read(ir::Reg::S(code));
        if (!lo) {
            return std::nullopt;
        }
        if (!p_->wave64) {
            return MaskKey{*lo, *lo};
        }
        const auto hi = inst.read(ir::Reg::S(static_cast<std::uint8_t>(code + 1)));
        if (!hi) {
            return std::nullopt;
        }
        return MaskKey{*lo, *hi};
    }

    std::optional<MaskKey> key_write(const ir::Inst& inst, std::uint8_t code) const
    {
        const auto lo = inst.write(ir::Reg::S(code));
        if (!lo) {
            return std::nullopt;
        }
        if (!p_->wave64) {
            return MaskKey{*lo, *lo};
        }
        const auto hi = inst.write(ir::Reg::S(static_cast<std::uint8_t>(code + 1)));
        if (!hi) {
            return std::nullopt;
        }
        return MaskKey{*lo, *hi};
    }

    const ir::Inst* def_of(const std::optional<std::pair<std::size_t, std::size_t>>& def) const
    {
        if (!def) {
            return nullptr;
        }
        return &p_->blocks[def->first].insts[def->second];
    }

    std::size_t born(ir::Value value) const
    {
        if (const auto found = defs_.find(value); found != defs_.end()) {
            return index_[found->second.first];
        }
        if (const auto found = phis_.find(value); found != phis_.end()) {
            return index_[found->second.first];
        }
        return 0;
    }

    Bool exec(const ir::Inst& inst)
    {
        if (const auto k = key_read(inst, ir::EXEC)) {
            return mask(*k).first;
        }
        return Bool::True;
    }

    std::pair<Bool, bool> mask(const MaskKey& k)
    {
        if (const auto found = masks_.find(k); found != masks_.end()) {
            return {found->second, true};
        }
        if (const auto found = other_.find(k); found != other_.end()) {
            return {found->second, false};
        }
        Bool m{};
        bool derived = false;
        if (const auto joined = join(k)) {
            m = joined->first;
            derived = joined->second;
        } else {
            const auto lo = [this](ir::Value v) -> std::optional<std::pair<std::size_t, std::size_t>> {
                const auto found = defs_.find(v);
                if (found == defs_.end()) {
                    return std::nullopt;
                }
                return found->second;
            };
            const auto hi = [this](ir::Value v) -> std::optional<std::pair<std::size_t, std::size_t>> {
                const auto found = defs_.find(v);
                if (found == defs_.end()) {
                    return std::nullopt;
                }
                return found->second;
            };
            const auto lo_def = lo(k.low);
            const auto hi_def = hi(k.high);
            std::optional<Bool> from_sop;
            if (lo_def && hi_def && *lo_def == *hi_def) {
                if (const ir::Inst* inst = def_of(lo_def)) {
                    if (const auto masks = sop(*inst); masks && masks->dst) {
                        from_sop = masks->dst->second;
                    }
                }
            }
            const auto exec_write = [this](const std::optional<std::pair<std::size_t, std::size_t>>& def,
                                           ir::Value value) {
                const ir::Inst* inst = def_of(def);
                if (inst == nullptr) {
                    return false;
                }
                return inst->write(ir::Reg::S(ir::EXEC)) == std::optional<ir::Value>{value}
                    || inst->write(ir::Reg::S(static_cast<std::uint8_t>(ir::EXEC + 1))) == std::optional<ir::Value>{value};
            };
            derived = exec_write(lo_def, k.low) || exec_write(hi_def, k.high);
            if (from_sop) {
                m = *from_sop;
            } else {
                m = b_.atom(Atom::Opaque(k.low), born(k.low));
            }
        }
        if (derived) {
            masks_.emplace(k, m);
        } else {
            other_.emplace(k, m);
        }
        return {m, derived};
    }

    std::optional<std::pair<Bool, bool>> join(const MaskKey& k)
    {
        const auto lo = [this](ir::Value v) -> std::optional<std::pair<std::size_t, std::size_t>> {
            const auto found = phis_.find(v);
            if (found == phis_.end()) {
                return std::nullopt;
            }
            return found->second;
        };
        const auto hi = [this](ir::Value v) -> std::optional<std::pair<std::size_t, std::size_t>> {
            const auto found = phis_.find(v);
            if (found == phis_.end()) {
                return std::nullopt;
            }
            return found->second;
        };
        const auto lo_phi = lo(k.low);
        const auto hi_phi = hi(k.high);
        std::size_t block = 0;
        if (lo_phi && hi_phi) {
            if (lo_phi->first != hi_phi->first) {
                return std::nullopt;
            }
            block = lo_phi->first;
        } else if (lo_phi) {
            block = lo_phi->first;
        } else if (hi_phi) {
            block = hi_phi->first;
        } else {
            return std::nullopt;
        }
        const ir::Block& target = p_->blocks[block];
        const auto arg = [](const std::optional<std::pair<std::size_t, std::size_t>>& phi,
                            ir::Value own, const ir::Block& block, std::size_t n) {
            return phi ? block.phis[phi->second].args[n] : own;
        };
        std::vector<MaskKey> args;
        args.reserve(target.preds.size());
        for (std::size_t n = 0; n < target.preds.size(); ++n) {
            args.push_back(MaskKey{arg(lo_phi, k.low, target, n),
                                   arg(hi_phi, k.high, target, n)});
        }
        const ir::Value id = lo_phi ? k.low : k.high;
        bool derived = derived_phis_.contains(id);
        std::vector<std::pair<Bool, Bool>> forward;
        for (std::size_t n = 0; n < args.size(); ++n) {
            const auto& [q, taken] = incoming_[block][n];
            if (index_[q] < index_[block]) {
                const auto [m, d] = mask(args[n]);
                derived = derived || d;
                const Bool e = edge(first_[q], q, taken);
                forward.push_back({e, m});
            } else {
                seen_.push_back(args[n]);
            }
        }
        if (forward.size() == args.size() && !forward.empty()) {
            const Bool m = forward.front().second;
            bool same = true;
            for (const auto& [e, mask_bool] : forward) {
                if (!(mask_bool == m)) {
                    same = false;
                    break;
                }
            }
            if (same) {
                return std::pair{m, derived};
            }
        }
        if (!unresolved_.contains(id)) {
            for (const bool value : {true, false}) {
                bool holds = true;
                for (const auto& [e, m] : forward) {
                    const Bool checked = value ? m : b_.not_(m);
                    if (!b_.implies(e, checked)) {
                        holds = false;
                        break;
                    }
                }
                if (holds) {
                    joins_.push_back(Join{block, id, value, derived, args});
                    return std::pair{value ? Bool::True : Bool::False, derived};
                }
            }
        }
        if (phi_join_ids_.insert(id).second) {
            phi_joins_.push_back(PhiJoin{block, id, derived, args});
        }
        return std::pair{b_.atom(Atom::Phi(id), index_[block]), derived};
    }

    std::optional<std::pair<Bool, bool>> operand(const ir::Inst& inst, const Operand& op)
    {
        const bool wave64 = p_->wave64;
        const auto constant = [this, wave64](std::uint64_t value) {
            if (!wave64) {
                value &= 0xFFFFFFFFull;
            }
            if (value == 0) {
                return Bool::False;
            }
            if (value == (wave64 ? 0xFFFFFFFFFFFFFFFFull : 0xFFFFFFFFull)) {
                return Bool::True;
            }
            return b_.atom(Atom::Constant(value), 0);
        };
        if (const auto* scalar = op.as<Operand::Scalar>();
            scalar != nullptr && scalar->code != NULL_REG) {
            const auto k = key_read(inst, scalar->code);
            if (!k) {
                return std::nullopt;
            }
            const auto [m, d] = mask(*k);
            return std::pair{m, d};
        }
        if (const auto* constant_operand = op.as<Operand::Const>()) {
            if (constant_operand->code >= 128 && constant_operand->code <= 208) {
                const std::int64_t value = constant_operand->code <= 192
                                               ? static_cast<std::int64_t>(constant_operand->code) - 128
                                               : 192 - static_cast<std::int64_t>(constant_operand->code);
                return std::pair{constant(static_cast<std::uint64_t>(value)), false};
            }
            return std::nullopt;
        }
        if (const auto* literal = op.as<Operand::Literal>()) {
            return std::pair{constant(literal->value), false};
        }
        return std::nullopt;
    }

    std::optional<SopMasks> sop(const ir::Inst& inst)
    {
        const auto* fields = inst.fields.as<Fields::Sop>();
        if (fields == nullptr) {
            return std::nullopt;
        }
        const std::string_view width = p_->wave64 ? "_b64" : "_b32";
        const auto name = strip_prefix(inst.name(), "s_");
        if (!name) {
            return std::nullopt;
        }
        const auto stripped = strip_suffix(*name, width);
        if (!stripped) {
            return std::nullopt;
        }
        const std::string_view op = *stripped;
        std::optional<MaskKey> dst;
        if (fields->dst) {
            if (const auto* scalar = fields->dst->as<Operand::Scalar>();
                scalar != nullptr && scalar->code != NULL_REG) {
                dst = key_write(inst, scalar->code);
            }
        }
        const auto exec_out = key_write(inst, ir::EXEC);
        std::optional<bool> save;
        std::string_view f = op;
        if (const auto stripped_save = strip_suffix(op, "_saveexec")) {
            save = true;
            f = *stripped_save;
        } else if (const auto stripped_write = strip_suffix(op, "_wrexec")) {
            save = false;
            f = *stripped_write;
        }
        bool derived = exec_out.has_value() || save.has_value();
        const auto arg = [&](std::size_t n) -> std::optional<Bool> {
            if (n >= fields->src.size()) {
                return std::nullopt;
            }
            const auto result = operand(inst, fields->src[n]);
            if (!result) {
                return std::nullopt;
            }
            derived = derived || result->second;
            return result->first;
        };
        if (save) {
            const auto s = arg(0);
            if (!s) {
                return std::nullopt;
            }
            const Bool exec_bits = exec(inst);
            const auto combined = combine(f, *s, exec_bits);
            if (!combined) {
                return std::nullopt;
            }
            SopMasks result{};
            result.derived = derived;
            result.scc = *combined;
            const Bool saved = *save ? exec_bits : *combined;
            if (dst) {
                result.dst = std::pair{*dst, saved};
            }
            if (exec_out) {
                result.exec = std::pair{*exec_out, *combined};
            }
            return result;
        }
        Bool value{};
        std::optional<Bool> scc;
        if (op == "mov") {
            const auto a = arg(0);
            if (!a) {
                return std::nullopt;
            }
            value = *a;
        } else if (op == "not") {
            const auto a = arg(0);
            if (!a) {
                return std::nullopt;
            }
            value = b_.not_(*a);
            scc = value;
        } else if (op == "wqm") {
            const auto a = arg(0);
            if (!a) {
                return std::nullopt;
            }
            if (!dst && !exec_out) {
                return std::nullopt;
            }
            const MaskKey low = dst ? *dst : *exec_out;
            const Bool quad = b_.atom(Atom::Quad(low.low), born(low.low));
            value = b_.or_(*a, quad);
            scc = value;
        } else if (op == "cselect") {
            const auto a = arg(0);
            const auto c = arg(1);
            if (!a || !c) {
                return std::nullopt;
            }
            const auto scc_value = inst.read(ir::Reg::Scc());
            if (!scc_value) {
                return std::nullopt;
            }
            if (clear_after_loop(*scc_value)) {
                value = *c;
            } else {
                const Bool uniform = b_.atom(Atom::Uniform(*scc_value), born(*scc_value));
                value = b_.ite(uniform, *a, *c);
            }
        } else {
            const auto a = arg(0);
            const auto c = arg(1);
            if (!a || !c) {
                return std::nullopt;
            }
            const auto combined = combine(op, *a, *c);
            if (!combined) {
                return std::nullopt;
            }
            value = *combined;
            scc = value;
        }
        SopMasks result{};
        result.derived = derived;
        result.scc = scc;
        if (dst) {
            result.dst = std::pair{*dst, value};
        }
        if (exec_out) {
            result.exec = std::pair{*exec_out, value};
        }
        return result;
    }

    bool clear_after_loop(ir::Value scc) const
    {
        const auto found = defs_.find(scc);
        if (found == defs_.end()) {
            return false;
        }
        const std::size_t block_index = found->second.first;
        const std::size_t inst_index = found->second.second;
        const ir::Block& block = p_->blocks[block_index];
        if (inst_index >= block.insts.size() || block.insts.empty()) {
            return false;
        }
        const ir::Inst& writer = block.insts[inst_index];
        const ir::Inst& last = block.insts.back();
        const auto exec_write = writer.write(ir::Reg::S(ir::EXEC));
        bool back = false;
        if (block.exit.kind == ir::Exit::Kind::Branch) {
            back = index_[block.exit.taken] <= index_[block_index];
        }
        return back && inst_index + 2 == block.insts.size() && exec_write.has_value()
            && last.name() == "s_cbranch_execnz"
            && last.read(ir::Reg::S(ir::EXEC)) == exec_write
            && (!p_->wave64
                || last.read(ir::Reg::S(static_cast<std::uint8_t>(ir::EXEC + 1)))
                       == writer.write(ir::Reg::S(static_cast<std::uint8_t>(ir::EXEC + 1))));
    }

    std::optional<Bool> combine(std::string_view f, Bool a, Bool c)
    {
        if (f == "and") {
            return b_.and_(a, c);
        }
        if (f == "or") {
            return b_.or_(a, c);
        }
        if (f == "xor") {
            return b_.xor_(a, c);
        }
        if (f == "andn2") {
            return b_.land_not(a, c);
        }
        if (f == "orn2") {
            return b_.lor_not(a, c);
        }
        if (f == "andn1") {
            return b_.and_(b_.not_(a), c);
        }
        if (f == "orn1") {
            return b_.or_(b_.not_(a), c);
        }
        if (f == "nand") {
            return b_.not_(b_.and_(a, c));
        }
        if (f == "nor") {
            return b_.not_(b_.or_(a, c));
        }
        if (f == "xnor") {
            return b_.not_(b_.xor_(a, c));
        }
        return std::nullopt;
    }

    void step(const ir::Inst& inst, std::optional<Bool> exec_bits)
    {
        const std::string_view name = inst.name();
        if (const auto* fields = inst.fields.as<Fields::Sop>()) {
            bool takes_mask = false;
            for (const Operand& source : fields->src) {
                const auto* scalar = source.as<Operand::Scalar>();
                if (scalar == nullptr || scalar->code == NULL_REG) {
                    continue;
                }
                const auto k = key_read(inst, scalar->code);
                if (!k) {
                    continue;
                }
                const bool phi = phis_.contains(k->low) || phis_.contains(k->high);
                takes_mask = takes_mask || masks_.contains(*k)
                    || (phi && mask(*k).second);
            }
            const bool touches_exec = inst.write(ir::Reg::S(ir::EXEC)).has_value()
                                   || inst.read(ir::Reg::S(ir::EXEC)).has_value();
            if (takes_mask || touches_exec) {
                if (const auto result = sop(inst); result && result->derived) {
                    std::vector<std::pair<MaskKey, Bool>> defined;
                    if (result->dst) {
                        defined.push_back(*result->dst);
                    }
                    if (result->exec) {
                        defined.push_back(*result->exec);
                    }
                    for (const auto& [k, m] : defined) {
                        masks_.emplace(k, m);
                        const bool exec_itself = result->exec && result->exec->first == k;
                        const bool known = exec_bits && b_.implies(m, *exec_bits);
                        if (exec_itself || known) {
                            active_.insert(k.low);
                        }
                    }
                    if (const auto scc = inst.write(ir::Reg::Scc()); scc && result->scc) {
                        scc_lane_.emplace(*scc, *result->scc);
                    }
                }
            }
            const std::string_view width = p_->wave64 ? "64" : "32";
            if (takes_mask) {
                const auto rest = strip_prefix(name, "s_cmp_");
                const auto scc = inst.write(ir::Reg::Scc());
                if (rest && scc) {
                    const auto tail = strip_suffix(*rest, width);
                    if (tail && (*tail == "eq_u" || *tail == "lg_u") && fields->src.size() == 2) {
                        const auto a = operand(inst, fields->src[0]);
                        const auto c = operand(inst, fields->src[1]);
                        if (a && c) {
                            const Bool ne = b_.xor_(a->first, c->first);
                            const bool equal = rest->starts_with("eq");
                            const Bool m = equal ? b_.not_(ne) : ne;
                            scc_lane_.emplace(*scc, m);
                            if (equal) {
                                scc_every_.insert(*scc);
                            }
                        }
                    }
                }
            }
            if (takes_mask && name.starts_with("s_bcnt1_i32_b")) {
                const auto scc = inst.write(ir::Reg::Scc());
                if (scc && fields->src.size() == 1) {
                    const auto source = operand(inst, fields->src[0]);
                    if (source && source->second) {
                        const Bool e = exec_bits.value_or(Bool::True);
                        const Bool own = b_.given(source->first, e);
                        bool uniform = true;
                        for (const Atom& atom : b_.support(own)) {
                            if (atom.kind != Atom::Kind::Uniform) {
                                uniform = false;
                                break;
                            }
                        }
                        if (b_.implies(source->first, e) && uniform) {
                            scc_lane_.emplace(*scc, own);
                        }
                    }
                }
            }
            return;
        }
        const auto* fields = inst.fields.as<Fields::Valu>();
        if (fields == nullptr) {
            return;
        }
        const Bool exec_bits_now = exec(inst);
        const auto lane = [this, exec_bits_now](MaskKey k) {
            const Bool atom = b_.atom(Atom::Lane(k.low), born(k.low));
            const Bool m = b_.and_(exec_bits_now, atom);
            masks_.emplace(k, m);
            active_.insert(k.low);
        };
        if ((name == "v_cndmask_b32" || name.find("_co_ci_") != std::string_view::npos)
            && fields->src.size() > 2) {
            const auto* scalar = fields->src[2].as<Operand::Scalar>();
            if (scalar != nullptr && scalar->code != NULL_REG) {
                if (const auto k = key_read(inst, scalar->code)) {
                    if (name == "v_cndmask_b32") {
                        mask(*k);
                    } else {
                        seen_.push_back(*k);
                    }
                }
            }
        }
        if (name == "v_div_fmas_f32" || name == "v_div_fmas_f64") {
            if (const auto k = key_read(inst, ir::VCC)) {
                mask(*k);
            }
        }
        if (name.starts_with("v_cmpx")) {
            if (const auto k = key_write(inst, ir::EXEC)) {
                lane(*k);
            }
            return;
        }
        if (name.starts_with("v_cmp") && fields->dst) {
            for (const Operand& op : {*fields->dst}) {
                if (const auto* scalar = op.as<Operand::Scalar>()) {
                    if (const auto k = key_write(inst, scalar->code)) {
                        lane(*k);
                    }
                }
            }
        }
        if (fields->carry) {
            for (const Operand& op : {*fields->carry}) {
                if (const auto* scalar = op.as<Operand::Scalar>()) {
                    if (const auto k = key_write(inst, scalar->code)) {
                        lane(*k);
                    }
                }
            }
        }
    }

    std::optional<bool> wave_kind(const ir::Inst& inst) const
    {
        const std::string_view name = inst.name();
        if (name == "s_cbranch_execnz" || name == "s_cbranch_vccnz") {
            return true;
        }
        if (name == "s_cbranch_execz" || name == "s_cbranch_vccz") {
            return false;
        }
        if (name == "s_cbranch_scc0" || name == "s_cbranch_scc1") {
            const auto scc = inst.read(ir::Reg::Scc());
            if (!scc || !scc_lane_.contains(*scc)) {
                return std::nullopt;
            }
            const bool every = scc_every_.contains(*scc);
            return (name == "s_cbranch_scc1") != every;
        }
        return std::nullopt;
    }

    Bool branch(const ir::Inst& inst)
    {
        const std::string_view name = inst.name();
        const Bool exec_bits = exec(inst);
        if (name == "s_cbranch_execz") {
            return b_.not_(exec_bits);
        }
        if (name == "s_cbranch_execnz") {
            return exec_bits;
        }
        if (name == "s_cbranch_vccz" || name == "s_cbranch_vccnz") {
            Bool vcc = Bool::False;
            if (const auto k = key_read(inst, ir::VCC)) {
                vcc = mask(*k).first;
            }
            const Bool any = b_.and_(vcc, exec_bits);
            return ends_with(name, "nz") ? any : b_.not_(any);
        }
        const auto scc = inst.read(ir::Reg::Scc());
        if (!scc) {
            return Bool::False;
        }
        Bool set{};
        if (const auto found = scc_lane_.find(*scc); found != scc_lane_.end()) {
            set = found->second;
        } else {
            set = b_.atom(Atom::Uniform(*scc), born(*scc));
        }
        return name == "s_cbranch_scc1" ? set : b_.not_(set);
    }

    std::expected<std::vector<Bool>, ShaderError> paths()
    {
        const std::size_t count = p_->blocks.size();
        std::vector<Bool> cond(count, Bool::False);
        if (!order_.empty()) {
            cond[order_.front()] = Bool::True;
        }
        for (std::size_t round = 0; round < kMaxRounds; ++round) {
            bool changed = false;
            for (std::size_t i = 1; i < order_.size(); ++i) {
                const std::size_t s = order_[i];
                Bool c = Bool::False;
                for (const auto& [q, taken] : incoming_[s]) {
                    Bool e = edge(cond[q], q, taken);
                    if (i <= index_[q]) {
                        std::unordered_map<std::uint32_t, Bool> memo;
                        e = b_.forget(e, i, memo);
                    }
                    c = b_.or_(c, e);
                }
                if (!(c == cond[s])) {
                    cond[s] = c;
                    changed = true;
                }
            }
            if (b_.overflowed()) {
                return std::unexpected(kTooLarge);
            }
            if (!changed) {
                return cond;
            }
        }
        return std::unexpected(ShaderError::Isa(0, "lane conditions do not settle"));
    }

private:
    const ir::Program* p_{nullptr};
    std::vector<std::size_t> order_;
    std::vector<std::size_t> index_;
    std::vector<std::vector<std::pair<std::size_t, bool>>> incoming_;
    std::unordered_map<ir::Value, std::pair<std::size_t, std::size_t>> defs_;
    std::unordered_map<ir::Value, std::pair<std::size_t, std::size_t>> phis_;
    std::unordered_set<ir::Value> unresolved_;
    std::unordered_set<ir::Value> derived_phis_;
    Bools b_;
    std::unordered_map<MaskKey, Bool, MaskKeyHash> masks_;
    std::unordered_map<MaskKey, Bool, MaskKeyHash> other_;
    std::vector<MaskKey> seen_;
    std::unordered_map<ir::Value, Bool> scc_lane_;
    std::vector<Join> joins_;
    std::vector<PhiJoin> phi_joins_;
    std::unordered_set<ir::Value> phi_join_ids_;
    std::vector<Bool> first_;
    std::vector<std::optional<Bool>> when_;
    std::unordered_set<ir::Value> active_;
    std::unordered_set<std::size_t> wide_;
    std::unordered_set<ir::Value> scc_every_;
    std::vector<std::optional<bool>> wave_any_;
};

}

std::expected<Structured, ShaderError> structure_wide(
    const ir::Program& program, const std::unordered_set<std::size_t>& wide)
{
    auto created = Lanes::create(program);
    if (!created) {
        return std::unexpected(created.error());
    }
    Lanes lanes = std::move(*created);
    lanes.set_wide(wide);
    for (;;) {
        auto execs = lanes.masks_in_order();
        auto cond = lanes.paths();
        if (!cond) {
            return std::unexpected(cond.error());
        }
        auto [wrong, new_derived] = lanes.check_joins(*cond);
        if (lanes.overflowed()) {
            return std::unexpected(kTooLarge);
        }
        if (wrong.empty() && new_derived.empty()) {
            return lanes.finish(execs, *cond);
        }
        lanes.adopt_unresolved(wrong, new_derived);
        lanes.reset();
    }
}

std::expected<Structured, ShaderError> structure(const ir::Program& program)
{
    return structure_wide(program, {});
}

}
