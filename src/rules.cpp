#include "ffcsync/rules.hpp"

#include <algorithm>

namespace ffcsync {

Reason check_static(const Catalog& catalog, const RegistrationRequest& req) {
    if (!catalog.config.window_open) return Reason::WindowClosed;
    const Student* student = catalog.find_student(req.student_id);
    if (!student) return Reason::UnknownStudent;
    const Section* section = catalog.find_section(req.section_id);
    if (!section) return Reason::UnknownSection;

    if (req.op == Op::Add) {
        const Course& course = *catalog.find_course(section->course);
        for (const auto& p : course.prereqs) {
            if (!student->completed.contains(p)) return Reason::PrereqNotMet;
        }
    }
    return Reason::Ok;
}

Reason check_mutable(const Catalog& catalog, const RegistrationState& state,
                     const RegistrationRequest& req) {
    const Section& section = *catalog.find_section(req.section_id);
    const StudentState& st = state.students.at(req.student_id);

    if (req.op == Op::Drop) {
        return st.sections.contains(section.id) ? Reason::Ok : Reason::NotRegistered;
    }

    if (st.sections.contains(section.id)) return Reason::AlreadyRegistered;
    for (const auto& other : st.sections) {
        if (catalog.find_section(other)->course == section.course) return Reason::CourseAlreadyTaken;
    }
    if (state.enrolled.at(section.id) >= section.capacity) return Reason::SectionFull;
    for (const auto& slot : section.slots) {
        if (st.slots.contains(slot)) return Reason::TimetableClash;
    }

    const Course& course = *catalog.find_course(section.course);
    if (st.credits + course.credits > catalog.config.max_credits) return Reason::CreditLimit;

    auto limit = catalog.config.bucket_limits.find(course.bucket);
    if (limit != catalog.config.bucket_limits.end()) {
        auto used = st.bucket_credits.find(course.bucket);
        CreditTenths current = used == st.bucket_credits.end() ? 0 : used->second;
        if (current + course.credits > limit->second) return Reason::BucketLimit;
    }
    return Reason::Ok;
}

void apply_change(const Catalog& catalog, RegistrationState& state, const RegistrationRequest& req) {
    const Section& section = *catalog.find_section(req.section_id);
    const Course& course = *catalog.find_course(section.course);
    StudentState& st = state.students.at(req.student_id);

    if (req.op == Op::Add) {
        st.sections.insert(section.id);
        st.credits += course.credits;
        st.bucket_credits[course.bucket] += course.credits;
        st.slots.insert(section.slots.begin(), section.slots.end());
        state.enrolled.at(section.id) += 1;
    } else {
        st.sections.erase(section.id);
        st.credits -= course.credits;
        if ((st.bucket_credits[course.bucket] -= course.credits) == 0) {
            st.bucket_credits.erase(course.bucket);
        }
        for (const auto& slot : section.slots) st.slots.erase(slot);
        state.enrolled.at(section.id) -= 1;
    }
}

RegistrationResult decide_and_commit(const Catalog& catalog, RegistrationState& state,
                                     std::uint64_t seq, const RegistrationRequest& req,
                                     Reason static_verdict) {
    RegistrationResult result{seq, req, Status::Rejected, static_verdict};
    if (static_verdict != Reason::Ok) return result;

    result.reason = check_mutable(catalog, state, req);
    if (result.reason == Reason::Ok) {
        apply_change(catalog, state, req);
        result.status = Status::Accepted;
    }
    return result;
}

}  // namespace ffcsync
