use std::fmt;

#[derive(Copy, Clone, Debug)]
pub struct QueryOptions {
    pub allow_get_raw: bool,
    pub allow_cache: bool,
    /// Should we decrease the limit of elements found when we have no
    /// subelements in the subquery? This should generally be set to true,
    /// as having it false could mean very expensive queries. The queries
    /// would be expensive because we could go through many many trees where the
    /// sub elements have no matches, hence the limit would not decrease and
    /// hence we would continue on the increasingly expensive query.
    pub decrease_limit_on_range_with_no_sub_elements: bool,
    pub error_if_intermediate_path_tree_not_present: bool,
}

impl fmt::Display for QueryOptions {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        writeln!(f, "QueryOptions {{")?;
        writeln!(f, "  allow_get_raw: {}", self.allow_get_raw)?;
        writeln!(f, "  allow_cache: {}", self.allow_cache)?;
        writeln!(
            f,
            "  decrease_limit_on_range_with_no_sub_elements: {}",
            self.decrease_limit_on_range_with_no_sub_elements
        )?;
        writeln!(
            f,
            "  error_if_intermediate_path_tree_not_present: {}",
            self.error_if_intermediate_path_tree_not_present
        )?;
        write!(f, "}}")
    }
}

impl Default for QueryOptions {
    fn default() -> Self {
        QueryOptions {
            allow_get_raw: false,
            allow_cache: true,
            decrease_limit_on_range_with_no_sub_elements: true,
            error_if_intermediate_path_tree_not_present: true,
        }
    }
}
