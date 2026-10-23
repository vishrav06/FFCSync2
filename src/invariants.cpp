#include "ffcsync/invariants.hpp"

#include <map>
#include <set>

namespace ffcsync {

std::vector<std::string> check_invariants(const Catalog& catalog, const RegistrationState& state) {
    std::vector<std::string> v;
    std::map<SectionId, int> counted;

    for (const auto& [sid, st] : state.students) {
        const std::string who = "student " + sid + ": ";
        const Student* student = catalog.find_student(sid);
        if (!student) {
            v.push_back(who + "not in catalog");
            continue;
        }
        CreditTenths credits = 0;
        std::map<Bucket, CreditTenths> buckets;
        std::set<CourseCode> courses;
        std::set<Slot> slots;
        std::size_t slot_count = 0;

        for (const auto& sec_id : st.sections) {
            const Section* section = catalog.find_section(sec_id);
            if (!section) {
                v.push_back(who + "registered in unknown section " + sec_id);
                continue;
            }
            ++counted[sec_id];
            const Course& course = *catalog.find_course(section->course);
            if (!courses.insert(course.code).second) {
                v.push_back(who + "registered twice for course " + course.code);
            }
            for (const auto& p : course.prereqs) {
                if (!student->completed.contains(p)) {
                    v.push_back(who + "missing prerequisite " + p + " for " + course.code);
                }
            }
            credits += course.credits;
            buckets[course.bucket] += course.credits;
            slots.insert(section->slots.begin(), section->slots.end());
            slot_count += section->slots.size();
        }

        if (credits != st.credits) {
            v.push_back(who + "stored credits " + std::to_string(st.credits) + " != recomputed " +
                        std::to_string(credits));
        }
        if (credits > catalog.config.max_credits) {
            v.push_back(who + "credits " + std::to_string(credits) + " exceed maximum");
        }
        if (buckets != st.bucket_credits) v.push_back(who + "stored bucket credits differ");
        for (const auto& [bucket, used] : buckets) {
            auto limit = catalog.config.bucket_limits.find(bucket);
            if (limit != catalog.config.bucket_limits.end() && used > limit->second) {
                v.push_back(who + "bucket " + bucket + " over limit");
            }
        }
        if (slot_count != slots.size()) v.push_back(who + "timetable clash between registered sections");
        if (slots != st.slots) v.push_back(who + "stored timetable slots differ");
    }

    for (const auto& [id, section] : catalog.sections) {
        auto it = state.enrolled.find(id);
        if (it == state.enrolled.end()) {
            v.push_back("section " + id + ": missing seat counter");
            continue;
        }
        const int enrolled = it->second;
        if (enrolled < 0 || enrolled > section.capacity) {
            v.push_back("section " + id + ": enrolled " + std::to_string(enrolled) +
                        " outside [0, " + std::to_string(section.capacity) + "]");
        }
        if (enrolled != counted[id]) {
            v.push_back("section " + id + ": enrolled " + std::to_string(enrolled) + " but " +
                        std::to_string(counted[id]) + " students registered");
        }
    }
    return v;
}

}  // namespace ffcsync
