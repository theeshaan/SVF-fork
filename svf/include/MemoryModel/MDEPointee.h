#ifndef MDE_PERSISTENT_POINTS_TO_H_
#define MDE_PERSISTENT_POINTS_TO_H_

#include "PointsTo.h"
#include "Util/GeneralType.h"
#define LHF_ENABLE_PERFORMANCE_METRICS
#include <lhf/lhf_common.hpp>
#include <lhf/lhf.hpp>

namespace lhf {

/**
 * @brief      The main LatticeHashForest structure.
 */
class PointeeMDE {
public:
	/**
	 * @brief      Index returned by an operation. Being defined inside the
	 *             class ensures type safety and possible future extensions.
	 */
	struct Index {
		IndexValue value;

		Index(IndexValue idx = EMPTY_SET_VALUE): value(idx) {}

		bool is_empty() const {
			return value == EMPTY_SET_VALUE;
		}

		bool operator==(const Index &b) const {
			return value == b.value;
		}

		bool operator!=(const Index &b) const {
			return value != b.value;
		}

		bool operator<(const Index &b) const {
			return value < b.value;
		}

		bool operator>(const Index &b) const {
			return value > b.value;
		}

		String to_string() const {
			return std::to_string(value);
		}

		friend std::ostream& operator<<(std::ostream& os, const Index& obj) {
			os << obj.to_string();
			return os;
		}

		struct Hash {
			Size operator()(const Index &idx) const {
				return DefaultHash<IndexValue>()(idx.value);
			}
		};
	};

	/**
	 * The canonical element of property sets. Changes based on the nesting
	 * behaviour supplied.
	 */
	using PropertyElement = SVF::NodeID;

	/**
	 * The storage structure for property elements. Currently implemented as
	 * sorted vectors.
	 */
	using PropertySet = SVF::PointsTo;

	struct PropertySetHash {
		Size operator()(const PropertySet *a) const {
			return std::hash<SVF::PointsTo>()(*a);
		}
	};

	struct PropertySetFullEqual {
		bool operator()(const PropertySet *a, const PropertySet *b) const {
			return *a == *b;
		}
	};

	/**
	 * The structure responsible for mapping property sets to their respective
	 * unique indices. When a key-value pair is actually inserted into the map,
	 * the key is a pointer to a valid storage location held by a member of
	 * the property set storage vector.
	 *
	 * @note The reason the 'key type' of the map is a pointer to a property set
	 *       is because of several reasons:
	 *
	 *       * Allows us to query arbitrary/user created property sets on the
	 *         map.
	 *       * Makes it not necessary to actually directly store the property
	 *         set as a key.
	 *       * Saves us from having to do some sort of complicated manuever to
	 *         reserve an index value temporarily and rewrite the hash and
	 *         equality comparators to retrieve the property sets from the
	 *         indices instead.
	 *
	 *       Careful handling, especially in the case of reallocating structures
	 *       like vectors is needed so that the address of the data does not
	 *       change. It must remain static for the duration of the existence of
	 *       the LHF instance.
	 */
#ifdef LHF_ENABLE_TBB
	using PropertySetMap =
		MapAdapter<tbb::concurrent_hash_map<
			const PropertySet *, IndexValue,
			TBBHashCompare<
				const PropertySet *,
				PropertySetHash,
				PropertySetFullEqual>>>;
#else
	using PropertySetMap =
		MapAdapter<std::unordered_map<
			const PropertySet *, IndexValue,
			PropertySetHash,
			PropertySetFullEqual>>;
#endif

	using UnaryOperationMap = OperationMap<IndexValue>;
	using BinaryOperationMap = OperationMap<OperationNode>;

protected:

#ifdef LHF_ENABLE_PERFORMANCE_METRICS
	PerformanceStatistics stat;
	HashMap<String, OperationPerf> perf;
#endif

	struct PropertySetHolder {
		using PtrContainer = UniquePointer<PropertySet>;
		using Ptr = typename PtrContainer::pointer;

		mutable PtrContainer ptr;

		PropertySetHolder(Ptr &&p): ptr(p) {}

		Ptr get() const {
			return ptr.get();
		}

		bool is_evicted() const {
#ifdef LHF_ENABLE_EVICTION
			return ptr.get() == nullptr;
#else
			return false;
#endif
		}

#ifdef LHF_ENABLE_EVICTION

		void evict() {
#if LHF_DEBUG
			if (!is_present()) {
				throw AssertError("Tried to evict an already absent property set");
			}
#endif
			ptr.release();
		}

		void reassign(Ptr &&p) {
#if LHF_DEBUG
			if (is_present()) {
				throw AssertError("Tried to reassign when a property set is already present");
			}
#endif
			ptr.reset(p);
		}

		void swap(PropertySetHolder &p) {
#if LHF_DEBUG
			if (is_present()) {
				throw AssertError("Tried to reassign when a property set is already present");
			} else if (!p.is_present()) {
				throw AssertError("Attempted to swap to a null pointer");
			}
#endif
			ptr.swap(p.ptr);
		}

#endif
	};

	class PropertySetStorage {
	protected:
		/// @note Not marking this as mutable will not allow us to get a
		///       non-const reference on index-based access. Non-constness
		//        is important for eviction to work.
		mutable Vector<PropertySetHolder> data = {};

	public:
		/**
		 * @brief      Retuns a mutable reference to the property set holder at
		 *             a given set index. This is useful for eviction based
		 *             functions.
		 *
		 * @param[in]  idx   Set index
		 *
		 * @return     A mutable propety set holder reference.
		 */
		PropertySetHolder &at_mutable(const Index &idx) const {
			return data.at(idx.value);
		}

		const PropertySetHolder &at(const Index &idx) const {
			return data.at(idx.value);
		}

		Index push_back(PropertySetHolder &&p) {
			data.push_back(std::move(p));
			return data.size() - 1;
		}

		void clear() {
			data.clear();
		}

		Vector<PropertySetHolder>::iterator begin() {
			return data.begin();
		}

		Vector<PropertySetHolder>::iterator end() {
			return data.end();
		}

		Size size() const {
			return data.size();
		}
	};

	// The property set storage array.
	PropertySetStorage property_sets = {};

	// The property set -> Index in storage array mapping.
	PropertySetMap property_set_map = {};

	BinaryOperationMap unions = {};
	BinaryOperationMap intersections = {};
	BinaryOperationMap differences = {};

	InternalMap<OperationNode, SubsetRelation> subsets = {};

	/**
	 * @brief      Stores index `a` as the subset of index `b` if a < b,
	 *             else stores index `a` as the superset of index `b`
	 *
	 * @param[in]  a     The index of the first set
	 * @param[in]  b     The index of the second set.
	 */
	void store_subset(const Index &a, const Index &b) {
		LHF_PROPERTY_SET_PAIR_VALID(a, b)
		LHF_PROPERTY_SET_PAIR_UNEQUAL(a, b)
		__lhf_calc_functime(stat);

		// We need to maintain the operation pair in index-order here as well.
		if (a > b) {
			subsets.insert({{b.value, a.value}, SUPERSET});
		} else {
			subsets.insert({{a.value, b.value}, SUBSET});
		}
	}

public:
	PointeeMDE() {
		// INSERT EMPTY SET AT INDEX 0
		register_set(PropertySet());
	}

	inline bool is_empty(const Index &i) const {
		return i.is_empty();
	}

	void PointsTo_checkAndRemapAll() {
		for (auto &i : property_sets) {
			i.get()->checkAndRemap();
		}

		property_set_map.clear();

		for (Size i = 0; i < property_sets.size(); i++) {
			property_set_map.insert({property_sets.at(i).get(), i});
		}
	}

	SVF::Map<PropertySet, unsigned> PointsTo_getAllPts() const {
		SVF::Map<PropertySet, unsigned> allPts;
		for (Size i = 0; i < property_sets.size(); i++) {
			allPts[*property_sets.at(i).get()] = i;
		}

		return allPts;
	}

	/**
	 * @brief      Removes all data from the LHF.
	 */
	virtual void clear() {
		property_sets.clear();
		property_set_map.clear();
		unions.clear();
		intersections.clear();
		differences.clear();
		subsets.clear();
	}

	/**
	 * @brief      Resets state of the LHF to the default.
	 */
	void clear_and_initialize() {
		clear();
		register_set({});
	}

	/**
	 * @brief      Returns whether we currently know whether a is a subset or a
	 *             superset of b.
	 *
	 * @param[in]  a     The first set
	 * @param[in]  b     The second set
	 *
	 * @return     Enum value telling if it's a subset, superset or unknown
	 */
	SubsetRelation is_subset(const Index &a, const Index &b) const {
		LHF_PROPERTY_SET_PAIR_VALID(a, b)

		auto i = subsets.find({a.value, b.value});

		if (!i.is_present()) {
			return UNKNOWN;
		} else {
			return i.get();
		}
	}

	/**
	 * @brief         Inserts a (or gets an existing) single-element set into
	 *                property set storage.
	 *
	 * @param[in]  c  The single-element property set.
	 *
	 * @return        Index of the newly created/existing set.
	 *
	 * @todo          Check whether the cache hit check can be removed.
	 */
	Index register_set_single(const PropertyElement &c) {
		__lhf_calc_functime(stat);

		PropertySet bv;
		bv.set(c);

		PropertySetHolder new_set = PropertySetHolder(new PropertySet{std::move(bv)});

		auto result = property_set_map.find(new_set.get());

		if (!result.is_present()) {
			LHF_PERF_INC(property_sets, cold_misses);

			Index ret = property_sets.push_back(std::move(new_set));
			property_set_map.insert(std::make_pair(property_sets.at(ret).get(), ret.value));

			return ret;
		}
		LHF_EVICTION(else if (is_evicted(result.get())) {
			property_sets.at_mutable(result.get()).swap(new_set);
			return Index(result.get());
		})
		else {
			LHF_PERF_INC(property_sets, hits);
			return Index(result.get());
		}
	}

	/**
	 * @brief      Inserts a (or gets an existing) single-element set into
	 *             the property set storage, and reports whether this set
	 *             was already  present or not.
	 *
	 * @param[in]  c     The single-element property set.
	 * @param[out] cold  Report if this was a cold miss.
	 *
	 * @return     Index of the newly created set.
	 */
	Index register_set_single(const PropertyElement &c, bool &cold) {
		__lhf_calc_functime(stat);

		PropertySet bv;
		bv.set(c);

		PropertySetHolder new_set = PropertySetHolder(new PropertySet{std::move(bv)});
		auto result = property_set_map.find(new_set.get());

		if (!result.is_present()) {
			LHF_PERF_INC(property_sets, cold_misses);

			Index ret = property_sets.push_back(std::move(new_set));
			property_set_map.insert(std::make_pair(property_sets.at(ret).get(), ret.value));

			cold = true;
			return ret;
		}
		LHF_EVICTION(else if (is_evicted(result.get())) {
			property_sets.at_mutable(result.get()).swap(new_set);
			cold = false;
			return Index(result.get());
		})
		else {
			LHF_PERF_INC(property_sets, hits);
			cold = false;
			return Index(result.get());
		}
	}

	Index register_set(const PropertySet &c) {
		__lhf_calc_functime(stat);

		auto result = property_set_map.find(&c);

		if (!result.is_present()) {
			LHF_PERF_INC(property_sets, cold_misses);

			PropertySetHolder new_set(new PropertySet(c));
			Index ret = property_sets.push_back(std::move(new_set));
			property_set_map.insert(std::make_pair(property_sets.at(ret).get(), ret.value));
			return ret;
		}
		LHF_EVICTION(else if (is_evicted(result.get())) {
			property_sets.at_mutable(result.get()).reassign(new PropertySet(c));
			return Index(result.get());
		})
		else {
			LHF_PERF_INC(property_sets, hits);
			return Index(result.get());
		}
	}

	template <bool disable_integrity_check = false>
	Index register_set(const PropertySet &c, bool &cold) {
		__lhf_calc_functime(stat);

		if (!disable_integrity_check) {
			LHF_PROPERTY_SET_INTEGRITY_VALID(c);
		}

		auto result = property_set_map.find(&c);

		if (!result.is_present()) {
			LHF_PERF_INC(property_sets, cold_misses);

			PropertySetHolder new_set(new PropertySet(c));
			Index ret = property_sets.push_back(std::move(new_set));
			property_set_map.insert(std::make_pair(property_sets.at(ret).get(), ret.value));

			cold = true;
			return ret;
		}
		LHF_EVICTION(else if (is_evicted(result.get())) {
			property_sets.at_mutable(result.get()).reassign(new PropertySet(c));
			cold = false;
			return Index(result.get());
		})
		else {
			LHF_PERF_INC(property_sets, hits);
			cold = false;
			return Index(result.get());
		}
	}

	Index register_set(PropertySet &&c) {
		__lhf_calc_functime(stat);

		auto result = property_set_map.find(&c);

		if (!result.is_present()) {
			LHF_PERF_INC(property_sets, cold_misses);

			PropertySetHolder new_set(new PropertySet(std::move(c)));
			Index ret = property_sets.push_back(std::move(new_set));
			property_set_map.insert(std::make_pair(property_sets.at(ret).get(), ret.value));
			return ret;
		}
		LHF_EVICTION(else if (is_evicted(result.get())) {
			property_sets.at_mutable(result.get()).reassign(new PropertySet(c));
			return Index(result.get());
		})
		else {
			LHF_PERF_INC(property_sets, hits);
			return Index(result.get());
		}
	}

	Index register_set(PropertySet &&c, bool &cold) {
		__lhf_calc_functime(stat);

		auto result = property_set_map.find(&c);

		if (!result.is_present()) {
			LHF_PERF_INC(property_sets, cold_misses);

			PropertySetHolder new_set(new PropertySet(std::move(c)));
			Index ret = property_sets.push_back(std::move(new_set));
			property_set_map.insert(std::make_pair(property_sets.at(ret).get(), ret.value));

			cold = true;
			return ret;
		}
		LHF_EVICTION(else if (is_evicted(result.get())) {
			property_sets.at_mutable(result.get()).reassign(new PropertySet(c));
			cold = false;
			return Index(result.get());
		})
		else {
			LHF_PERF_INC(property_sets, hits);
			cold = false;
			return Index(result.get());
		}
	}

	template<typename Iterator>
	Index register_set(Iterator begin, Iterator end) {
		__lhf_calc_functime(stat);

		PropertySetHolder new_set(new PropertySet(begin, end));

		auto result = property_set_map.find(new_set.get());

		if (!result.is_present()) {
			LHF_PERF_INC(property_sets, cold_misses);

			Index ret = property_sets.push_back(std::move(new_set));
			property_set_map.insert(std::make_pair(property_sets.at(ret).get(), ret.value));
			return ret;
		}
		LHF_EVICTION(else if (is_evicted(result.get())) {
			property_sets.at_mutable(result.get()).reassign(std::move(new_set));
			return Index(result.get());
		})
		else {
			LHF_PERF_INC(property_sets, hits);
			return Index(result.get());
		}
	}

	template<typename Iterator>
	Index register_set(Iterator begin, Iterator end, bool &cold) {
		__lhf_calc_functime(stat);

		PropertySetHolder new_set(new PropertySet(begin, end));
		auto result = property_set_map.find(new_set.get());

		if (!result.is_present()) {
			LHF_PERF_INC(property_sets, cold_misses);

			Index ret = property_sets.push_back(std::move(new_set));
			property_set_map.insert(std::make_pair(property_sets.at(ret).get(), ret.value));
			cold = true;
			return ret;
		}
		LHF_EVICTION(else if (is_evicted(result.get())) {
			property_sets.at_mutable(result.get()).reassign(std::move(new_set));
			cold = false;
			return Index(result.get());
		})
		else {
			LHF_PERF_INC(property_sets, hits);
			cold = false;
			return Index(result.get());
		}
	}

#ifdef LHF_ENABLE_EVICTION
	bool is_evicted(const Index &index) const {
		return property_sets.at(index.value).is_evicted();
	}
#endif

#ifdef LHF_ENABLE_EVICTION
	void evict_set(const Index &index) {
#ifdef LHF_DEBUG
		if (index.is_empty()) {
			throw AssertError("Tried to evict the empty set");
		}
#endif
		property_sets.at_mutable(index.value).evict();
	}
#endif

	/**
	 * @brief      Gets the actual property set specified by index.
	 *
	 * @param[in]  index  The index
	 *
	 * @return     The property set.
	 */
	inline const PropertySet &get_value(const Index &index) const {
		LHF_PROPERTY_SET_INDEX_VALID(index);
#if defined(LHF_DEBUG) && defined(LHF_ENABLE_EVICTION)
		if (is_evicted(index)) {
			throw AssertError("Tried to access an evicted set");
		}
#endif
		return *property_sets.at(index.value).get();
	}

	/**
	 * @brief      Returns the total number of property sets currently in the
	 *             LHF.
	 *
	 * @return     The count.
	 */
	inline Size property_set_count() const {
		return property_sets.size();
	}

	/**
	 * @brief      Returns the size of the set at `index`
	 *
	 * @param[in]  index  The index
	 *
	 * @return     size of the set.
	 */
	inline Size size_of(const Index &index) const {
		if (index == EMPTY_SET_VALUE) {
			return 0;
		} else {
			return get_value(index).count();
		}
	}

	/**
	 * @brief      Determines whether the property set at `index` contains the
	 *             element `prop` or not.
	 *
	 * @param[in]  index  The index
	 * @param[in]  prop   The property
	 *
	 * @return     `true` if `prop` is in `index`, false otherwise.
	 */
	inline bool contains(const Index &index, const PropertyElement &prop) const {
		return get_value(index).test(prop);
	}

	/**
	 * @brief      Calculates, or returns a cached result of the union
	 *             of `a` and `b`
	 *
	 * @param[in]  a     The first set
	 * @param[in]  b     The second set
	 *
	 * @return     Index of the new property set.
	 */
	Index set_union(const Index &_a, const Index &_b) {
		LHF_PROPERTY_SET_PAIR_VALID(_a, _b);
		__lhf_calc_functime(stat);

		if (_a == _b) {
			LHF_PERF_INC(unions, equal_hits);
			return Index(_a);
		}

		if (is_empty(_a)) {
			LHF_PERF_INC(unions, empty_hits);
			return Index(_b);
		} else if (is_empty(_b)) {
			LHF_PERF_INC(unions,empty_hits);
			return Index(_a);
		}

		const Index &a = std::min(_a, _b);
		const Index &b = std::max(_a, _b);

		SubsetRelation r = is_subset(a, b);

		if (r == SUBSET) {
			LHF_PERF_INC(unions, subset_hits);
			return Index(b);
		} else if (r == SUPERSET) {
			LHF_PERF_INC(unions, subset_hits);
			return Index(a);
		}

		auto result = unions.find({a.value, b.value});

		if (!result.is_present() LHF_EVICTION(|| is_evicted(result.get()))) {
			PropertySet new_set = get_value(a) | get_value(b);

			bool cold = false;
			Index ret;

			LHF_EVICTION(if (result.is_present() && is_evicted(result.get())) {
				ret = result.get();
				property_sets.at_mutable(ret).reassign(new PropertySet(std::move(new_set)));
			} else) {
				ret = register_set(std::move(new_set), cold);

				unions.insert({{a.value, b.value}, ret.value});

				if (ret == a) {
					store_subset(b, ret);
				} else if (ret == b) {
					store_subset(a, ret);
				} else {
					store_subset(a, ret);
					store_subset(b, ret);
				}
			}

			if (cold) {
				LHF_PERF_INC(unions, cold_misses);
			} else {
				LHF_PERF_INC(unions, edge_misses);
			}

			return Index(ret);
		} else {
			LHF_PERF_INC(unions, hits);
			return Index(result.get());
		}
	}

	/**
	 * @brief      Inserts a single element from a given set (and returns the
	 *             index of the set). This is a wrapper over the union
	 *             operation.
	 *
	 * @param[in]  a     The set to insert the element to
	 * @param[in]  b     The element to be inserted.
	 *
	 * @return     Index of the new PropertySet.
	 */
	Index set_insert_single(const Index &a, const PropertyElement &b) {
		return set_union(a, register_set_single(b));
	}

	/**
	 * @brief      Calculates, or returns a cached result of the difference
	 *             of `a` from `b`
	 *
	 * @param[in]  a     The first set (what to subtract from)
	 * @param[in]  b     The second set (what will be subtracted)
	 *
	 * @return     Index of the new PropertySet.
	 */
	Index set_difference(const Index &a, const Index &b) {
		LHF_PROPERTY_SET_PAIR_VALID(a, b);
		__lhf_calc_functime(stat);

		if (a == b) {
			LHF_PERF_INC(differences, equal_hits);
			return Index(EMPTY_SET_VALUE);
		}

		if (is_empty(a)) {
			LHF_PERF_INC(differences, empty_hits);
			return Index(EMPTY_SET_VALUE);
		} else if (is_empty(b)) {
			LHF_PERF_INC(differences, empty_hits);
			return Index(a);
		}

		auto result = differences.find({a.value, b.value});

		if (!result.is_present() LHF_EVICTION(|| is_evicted(result.get()))) {
			PropertySet new_set = get_value(a) - get_value(b);

			bool cold = false;
			Index ret;

			LHF_EVICTION(if (result.is_present() && is_evicted(result.get())) {
				ret = result.get();
				property_sets.at_mutable(ret).reassign(new PropertySet(std::move(new_set)));
			} else) {
				ret = register_set(std::move(new_set), cold);
				differences.insert({{a.value, b.value}, ret.value});

				if (ret != a) {
					store_subset(ret, a);
				} else {
					intersections.insert({
						{
							std::min(a.value, b.value),
							std::max(a.value, b.value)
						}, EMPTY_SET_VALUE});
				}
			}

			if (cold) {
				LHF_PERF_INC(differences, cold_misses);
			} else {
				LHF_PERF_INC(differences, edge_misses);
			}

			return Index(ret);
		} else {
			LHF_PERF_INC(differences, hits);
			return Index(result.get());
		}
	}

	/**
	 * @brief      Removes a single element from a given set (and returns the
	 *             index of the set). This is a wrapper over the diffrerence
	 *             operation.
	 *
	 * @param[in]  a     The set to remove the element from
	 * @param[in]  b     The element to be removed
	 *
	 * @return     Index of the new PropertySet.
	 */
	Index set_remove_single(const Index &a, const PropertyElement &b) {
		return set_difference(a, register_set_single(b));
	}

	/**
	 * @brief      Calculates, or returns a cached result of the intersection
	 *             of `a` and `b`
	 *
	 * @param[in]  a     The first set
	 * @param[in]  b     The second set
	 *
	 * @return     Index of the new property set.
	 */
	Index set_intersection(const Index &_a, const Index &_b) {
		LHF_PROPERTY_SET_PAIR_VALID(_a, _b);
		__lhf_calc_functime(stat);

		if (_a == _b) {
			LHF_PERF_INC(intersections, equal_hits);
			return Index(_a);
		}

		if (is_empty(_a) || is_empty(_b)) {
			LHF_PERF_INC(intersections, empty_hits);
			return Index(EMPTY_SET_VALUE);
		}

		const Index &a = std::min(_a, _b);
		const Index &b = std::max(_a, _b);

		SubsetRelation r = is_subset(a, b);

		if (r == SUBSET) {
			LHF_PERF_INC(intersections, subset_hits);
			return Index(a);
		} else if (r == SUPERSET) {
			LHF_PERF_INC(intersections, subset_hits);
			return Index(b);
		}

		auto result = intersections.find({a.value, b.value});

		if (!result.is_present() LHF_EVICTION(|| is_evicted(result.get()))) {
			PropertySet new_set = get_value(a) & get_value(b);

			bool cold = false;
			Index ret;

			LHF_EVICTION(if (result.is_present() && is_evicted(result.get())) {
				ret = result.get();
				property_sets.at_mutable(ret).reassign(new PropertySet(std::move(new_set)));
			} else){
				ret = register_set(std::move(new_set), cold);
				intersections.insert({{a.value, b.value}, ret.value});

				if (ret != a) {
					store_subset(ret, a);
				} else if (ret != b) {
					store_subset(ret, b);
				} else {
					store_subset(ret, a);
					store_subset(ret, b);
				}
			}

			if (cold) {
				LHF_PERF_INC(intersections, cold_misses);
			} else {
				LHF_PERF_INC(intersections, edge_misses);
			}
			return Index(ret);
		}

		LHF_PERF_INC(intersections, hits);
		return Index(result.get());
	}


	/**
	 * @brief      Filters a set based on a criterion function.
	 *             This is supposed to be an abstract filtering mechanism that
	 *             derived classes will use to implement caching on a filter
	 *             operation rather than letting them implement their own.
	 *
	 * @param[in]  s            The set to filter
	 * @param[in]  filter_func  The filter function (can be a lambda)
	 * @param      cache        The cache to use (possibly defined by the user)
	 *
	 * @tparam     is_sort_bounded  Useful for telling the function that the
	 *                              filter criterion will have a lower and an
	 *                              upper bound in a sorted list. This can
	 *                              potentially result in a faster filtering.
	 *
	 * @todo Implement sort bound optimization
	 * @todo Implement bounding as a separate function instead
	 *
	 * @return     Index of the filtered set.
	 */
	Index set_filter(
		Index s,
		std::function<bool(const PropertyElement &)> filter_func,
		UnaryOperationMap &cache) {
		LHF_PROPERTY_SET_INDEX_VALID(s);
		__lhf_calc_functime(stat);

		if (is_empty(s)) {
			return s;
		}

		auto result = cache.find(s.value);

		if (!result.is_present() LHF_EVICTION(|| is_evicted(result.get()))) {
			PropertySet new_set;
			for (const PropertyElement &value : get_value(s)) {
				if (filter_func(value)) {
					new_set.set(value);
				}
			}

			bool cold;
			Index ret;

			LHF_EVICTION(if (result.is_present() && is_evicted(result.get())) {
				ret = result.get();
				property_sets.at_mutable(ret).reassign(new PropertySet(std::move(new_set)));
			} else) {
				ret = register_set(std::move(new_set), cold);
				cache.insert(std::make_pair(s.value, ret.value));
			}

			if (cold) {
				LHF_PERF_INC(filter, cold_misses);
			} else {
				LHF_PERF_INC(filter, edge_misses);
			}

			return Index(ret);
		} else {
			LHF_PERF_INC(filter, hits);
			return Index(result.get());
		}
	}

	/**
	 * @brief      Converts the property set to a string.
	 *
	 * @param[in]  set   The set
	 *
	 * @return     The string representation of the set.
	 */
	static String property_set_to_string(const PropertySet &set) {
		std::stringstream s;
		s << "{ ";
		for (auto value : set) {
			s << value << " ";
		}
		s << "}";
		return s.str();
	}

	String property_set_to_string(const Index &idx) const {
		return property_set_to_string(get_value(idx));
	}

	/**
	 * @brief      Dumps the current state of the LHF to a string.
	 */
	String dump() const {
		std::stringstream s;

		s << "{\n";

		s << "    " << "Unions: " << "(Count: " << unions.size() << ")\n";
		s << unions.to_string();
		s << "\n";

		s << "    " << "Differences:" << "(Count: " << differences.size() << ")\n";
		s << differences.to_string();
		s << "\n";

		s << "    " << "Intersections: " << "(Count: " << intersections.size() << ")\n";
		s << intersections.to_string();
		s << "\n";

		s << "    " << "Subsets: " << "(Count: " << subsets.size() << ")\n";
		for (auto i : subsets) {
			s << "      " << i.first << " -> " << (i.second == SUBSET ? "sub" : "sup") << "\n";
		}
		s << "\n";

		s << "    " << "PropertySets: " << "(Count: " << property_sets.size() << ")\n";
		for (size_t i = 0; i < property_sets.size(); i++) {
			s << "      "
				<< i << " : " << property_set_to_string(*property_sets.at(i).get()) << "\n";
		}
		s << "}\n";

		return s.str();
	}

	friend std::ostream& operator<<(std::ostream& os, const PointeeMDE& obj) {
		os << obj.dump();
		return os;
	}

#ifdef LHF_ENABLE_PERFORMANCE_METRICS
	/**
	 * @brief      Dumps performance information as a string.
	 * @note       Conditionally enabled if `LHF_ENABLE_PERFORMANCE_METRICS` is
	 *             set.
	 * @return     String containing performance information as a human-readable
	 *             string.
	 */
	String dump_perf() const {
		std::stringstream s;
		s << "Performance Profile: \n";
		for (auto &p : perf) {
			s << p.first << "\n"
			  << p.second.to_string() << "\n";
		}
		s << stat.dump();
		return s.str();
	}
#endif

}; // END LatticeHashForest


}; // END NAMESPACE

#endif