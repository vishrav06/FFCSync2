#include "ffcsync/model.hpp"

#include <array>
#include <utility>

namespace ffcsync {

namespace {

constexpr std::array<std::pair<Reason, std::string_view>, 13> kReasonNames{{
    {Reason::Ok, "OK"},
    {Reason::Malformed, "MALFORMED"},
    {Reason::WindowClosed, "WINDOW_CLOSED"},
    {Reason::UnknownStudent, "UNKNOWN_STUDENT"},
    {Reason::UnknownSection, "UNKNOWN_SECTION"},
    {Reason::PrereqNotMet, "PREREQ_NOT_MET"},
    {Reason::AlreadyRegistered, "ALREADY_REGISTERED"},
    {Reason::CourseAlreadyTaken, "COURSE_ALREADY_TAKEN"},
    {Reason::SectionFull, "SECTION_FULL"},
    {Reason::TimetableClash, "TIMETABLE_CLASH"},
    {Reason::CreditLimit, "CREDIT_LIMIT"},
    {Reason::BucketLimit, "BUCKET_LIMIT"},
    {Reason::NotRegistered, "NOT_REGISTERED"},
}};

}  // namespace

std::string_view to_string(Op op) { return op == Op::Add ? "ADD" : "DROP"; }

std::string_view to_string(Status status) {
    return status == Status::Accepted ? "ACCEPTED" : "REJECTED";
}

std::string_view to_string(Reason reason) {
    for (const auto& [r, name] : kReasonNames) {
        if (r == reason) return name;
    }
    return "UNKNOWN";
}

std::optional<Op> op_from_string(std::string_view s) {
    if (s == "ADD") return Op::Add;
    if (s == "DROP") return Op::Drop;
    return std::nullopt;
}

std::optional<Status> status_from_string(std::string_view s) {
    if (s == "ACCEPTED") return Status::Accepted;
    if (s == "REJECTED") return Status::Rejected;
    return std::nullopt;
}

std::optional<Reason> reason_from_string(std::string_view s) {
    for (const auto& [r, name] : kReasonNames) {
        if (name == s) return r;
    }
    return std::nullopt;
}

const Student* Catalog::find_student(const StudentId& id) const {
    auto it = students.find(id);
    return it == students.end() ? nullptr : &it->second;
}

const Section* Catalog::find_section(const SectionId& id) const {
    auto it = sections.find(id);
    return it == sections.end() ? nullptr : &it->second;
}

const Course* Catalog::find_course(const CourseCode& code) const {
    auto it = courses.find(code);
    return it == courses.end() ? nullptr : &it->second;
}

RegistrationState RegistrationState::empty_for(const Catalog& catalog) {
    RegistrationState state;
    for (const auto& [id, section] : catalog.sections) state.enrolled[id] = 0;
    for (const auto& [id, student] : catalog.students) state.students[id] = StudentState{};
    return state;
}

}  // namespace ffcsync
