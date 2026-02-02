use std::fmt;

use grovedb_element::Element;
use grovedb_merk::proofs::{
    query::{Path, SubqueryBranch},
    Query,
};
use grovedb_storage::rocksdb_storage::RocksDbStorage;

use crate::{
    element::query_options::QueryOptions,
    operations::proof::util::hex_to_ascii,
    query_result_type::{QueryResultElement, QueryResultType},
    TransactionArg,
};

/// Path query push arguments
pub struct PathQueryPushArgs<'db, 'ctx, 'a>
where
    'db: 'ctx,
{
    pub storage: &'db RocksDbStorage,
    pub transaction: TransactionArg<'db, 'ctx>,
    pub key: Option<&'a [u8]>,
    pub element: Element,
    pub path: &'a [&'a [u8]],
    pub subquery_path: Option<Path>,
    pub subquery: Option<Query>,
    pub left_to_right: bool,
    pub query_options: QueryOptions,
    pub result_type: QueryResultType,
    pub results: &'a mut Vec<QueryResultElement>,
    pub limit: &'a mut Option<u16>,
    pub offset: &'a mut Option<u16>,
}

fn format_query(query: &Query, indent: usize) -> String {
    let indent_str = " ".repeat(indent);
    let mut output = format!("{}Query {{\n", indent_str);

    output += &format!("{}  items: [\n", indent_str);
    for item in &query.items {
        output += &format!("{}    {},\n", indent_str, item);
    }
    output += &format!("{}  ],\n", indent_str);

    output += &format!(
        "{}  default_subquery_branch: {}\n",
        indent_str,
        format_subquery_branch(&query.default_subquery_branch, indent + 2)
    );

    if let Some(ref branches) = query.conditional_subquery_branches {
        output += &format!("{}  conditional_subquery_branches: {{\n", indent_str);
        for (item, branch) in branches {
            output += &format!(
                "{}    {}: {},\n",
                indent_str,
                item,
                format_subquery_branch(branch, indent + 4)
            );
        }
        output += &format!("{}  }},\n", indent_str);
    }

    output += &format!("{}  left_to_right: {}\n", indent_str, query.left_to_right);
    output += &format!("{}}}", indent_str);

    output
}

fn format_subquery_branch(branch: &SubqueryBranch, indent: usize) -> String {
    let indent_str = " ".repeat(indent);
    let mut output = "SubqueryBranch {{\n".to_string();

    if let Some(ref path) = branch.subquery_path {
        output += &format!("{}  subquery_path: {:?},\n", indent_str, path);
    }

    if let Some(ref subquery) = branch.subquery {
        output += &format!(
            "{}  subquery: {},\n",
            indent_str,
            format_query(subquery, indent + 2)
        );
    }

    output += &format!("{}}}", " ".repeat(indent));

    output
}

impl<'db, 'ctx> fmt::Display for PathQueryPushArgs<'db, 'ctx, '_>
where
    'db: 'ctx,
{
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        writeln!(f, "PathQueryPushArgs {{")?;
        writeln!(
            f,
            "  key: {}",
            self.key.map_or("None".to_string(), hex_to_ascii)
        )?;
        writeln!(f, "  element: {}", self.element)?;
        writeln!(
            f,
            "  path: [{}]",
            self.path
                .iter()
                .map(|p| hex_to_ascii(p))
                .collect::<Vec<_>>()
                .join(", ")
        )?;
        writeln!(
            f,
            "  subquery_path: {}",
            self.subquery_path
                .as_ref()
                .map_or("None".to_string(), |p| format!(
                    "[{}]",
                    p.iter()
                        .map(|e| hex_to_ascii(e.as_slice()))
                        .collect::<Vec<_>>()
                        .join(", ")
                ))
        )?;
        writeln!(
            f,
            "  subquery: {}",
            self.subquery
                .as_ref()
                .map_or("None".to_string(), |q| format!("\n{}", format_query(q, 4)))
        )?;
        writeln!(f, "  left_to_right: {}", self.left_to_right)?;
        writeln!(f, "  query_options: {}", self.query_options)?;
        writeln!(f, "  result_type: {}", self.result_type)?;
        writeln!(
            f,
            "  results: [{}]",
            self.results
                .iter()
                .map(|r| format!("{}", r))
                .collect::<Vec<_>>()
                .join(", ")
        )?;
        writeln!(f, "  limit: {:?}", self.limit)?;
        writeln!(f, "  offset: {:?}", self.offset)?;
        write!(f, "}}")
    }
}
